import Foundation
import LuaRuntime
import PocketDSHCore

@MainActor
final class PluginRuntimeManager: ObservableObject {
    @Published private(set) var toolSpecs: [LLMToolSpec] = []
    @Published private(set) var loadedPluginIDs: Set<String> = []
    @Published private(set) var loadErrors: [String: String] = [:]

    private let store: PluginStore
    private let workspace: WorkspaceFileSystem
    private var runtimes: [String: LuaPluginRuntime] = [:]
    /// Python plugins, kept apart from the Lua ones: their calls are asynchronous and go through
    /// a guest process rather than into an embedded interpreter.
    private var pythonRuntimes: [String: PythonPluginRuntime] = [:]
    /// Supplied by the app when Tier D is available. Absent means Python plugins cannot load,
    /// which is stated rather than being a mystery failure.
    weak var pythonExecutor: PythonPluginExecuting?
    private var routes: [String: Route] = [:]

    init(store: PluginStore, workspace: WorkspaceFileSystem) {
        self.store = store
        self.workspace = workspace
        reload()
    }

    func reload() {
        for runtime in runtimes.values {
            runtime.cancel()
        }
        runtimes.removeAll()
        routes.removeAll()
        toolSpecs = []
        loadedPluginIDs = []
        loadErrors = [:]

        for plugin in store.plugins where plugin.enabled {
            let id = plugin.manifest.id
            let entry = store.entryURL(for: plugin)
            guard FileManager.default.fileExists(atPath: entry.path) else {
                let message = "Plugin entry not found: \(plugin.manifest.entry)"
                loadErrors[id] = message
                store.setError(message, for: id)
                continue
            }
            // A Python plugin goes to the guest CPython. Everything else still goes to Lua, and
            // says so rather than being parsed as Lua and failing somewhere unhelpful.
            let runtimeKind = (plugin.manifest.runtime ?? "lua").lowercased()
            if runtimeKind == "python" {
                guard let executor = pythonExecutor else {
                    let message = "This plugin runs on Python, which needs Tier D. Start Tier D, "
                        + "or install the Lua version."
                    loadErrors[id] = message
                    store.setError(message, for: id)
                    continue
                }
                let py = PythonPluginRuntime(identifier: id, executor: executor)
                do {
                    try py.load(entryURL: entry)
                    pythonRuntimes[id] = py
                    loadedPluginIDs.insert(id)
                    store.clearError(for: id)
                    registerTools(for: plugin, id: id)
                } catch {
                    let message = error.localizedDescription
                    loadErrors[id] = message
                    store.setError(message, for: id)
                }
                continue
            }
            guard runtimeKind == "lua" else {
                let message = "This plugin asks for the \"\(runtimeKind)\" runtime. This build "
                    + "runs Lua plugins and Python plugins; nothing else."
                loadErrors[id] = message
                store.setError(message, for: id)
                continue
            }

            let runtime = LuaPluginRuntime(identifier: id)
            let host = PluginWorkspaceHost(
                pluginID: id,
                manifest: plugin.manifest,
                workspace: workspace,
                store: store
            )
            do {
                try runtime.load(
                    entryURL: entry,
                    config: plugin.config,
                    host: host
                )
                runtimes[id] = runtime
                loadedPluginIDs.insert(id)
                store.clearError(for: id)

                registerTools(for: plugin, id: id)
            } catch {
                let message = error.localizedDescription
                loadErrors[id] = message
                store.setError(message, for: id)
            }
        }

        toolSpecs.sort { $0.function.name < $1.function.name }
    }

    /// One place that turns a manifest's tools into routes and specs, for every runtime.
    private func registerTools(for plugin: InstalledPlugin, id: String) {
        for tool in plugin.manifest.tools {
            let composite = Self.compositeToolName(pluginID: id, toolName: tool.name)
            routes[composite] = Route(pluginID: id, originalToolName: tool.name)
            toolSpecs.append(
                LLMToolSpec(
                    function: .init(
                        name: composite,
                        description: "[Plugin: \(plugin.manifest.name)] \(tool.description)",
                        parameters: tool.parameters
                    )
                )
            )
        }
    }

    func invoke(
        toolName: String,
        argumentsJSON: String
    ) async -> ToolExecutionResult? {
        guard let route = routes[toolName] else { return nil }
        // A Python plugin runs a guest process, so its result arrives later; a Lua plugin calls
        // straight into the embedded interpreter.
        if let pythonRuntime = pythonRuntimes[route.pluginID] {
            guard let arguments = decodeArguments(argumentsJSON) else {
                return .failure("Plugin tool arguments must be a JSON object.")
            }
            if Task.isCancelled {
                return .failure("Cancelled before plugin execution.")
            }
            let encoded = (try? JSONEncoder().encode(arguments)).flatMap {
                String(data: $0, encoding: .utf8)
            } ?? "{}"
            let outcome = await pythonRuntime.invoke(
                tool: route.originalToolName,
                argumentsJSON: encoded
            )
            if outcome.isError {
                store.log(pluginID: route.pluginID, level: .error, message: outcome.content)
                return .failure(outcome.content)
            }
            return .success(outcome.content)
        }
        guard let runtime = runtimes[route.pluginID] else {
            return nil
        }
        guard let arguments = decodeArguments(argumentsJSON) else {
            return .failure("Plugin tool arguments must be a JSON object.")
        }
        if Task.isCancelled {
            return .failure("Cancelled before plugin execution.")
        }

        let result = runtime.invoke(
            tool: route.originalToolName,
            arguments: arguments
        )
        if result.isError {
            store.log(
                pluginID: route.pluginID,
                level: .error,
                message: "\(route.originalToolName): \(result.content)"
            )
        } else {
            store.log(
                pluginID: route.pluginID,
                level: .info,
                message: "\(route.originalToolName): ok"
            )
        }
        return ToolExecutionResult(content: result.content, isError: result.isError)
    }

    func cancel() {
        for runtime in runtimes.values {
            runtime.cancel()
        }
    }

    func isPluginTool(_ name: String) -> Bool {
        routes[name] != nil
    }

    static func compositeToolName(pluginID: String, toolName: String) -> String {
        let safeID = pluginID.map { character -> Character in
            character.isLetter || character.isNumber || character == "_"
                ? character
                : "_"
        }
        return "plugin__\(String(safeID))__\(toolName)"
    }

    private func decodeArguments(_ json: String) -> [String: JSONValue]? {
        guard let data = json.data(using: .utf8),
              let value = try? JSONDecoder().decode(JSONValue.self, from: data) else {
            return nil
        }
        return value.objectValue
    }

    private struct Route {
        let pluginID: String
        let originalToolName: String
    }
}

private final class PluginWorkspaceHost: LuaPluginHost {
    private let pluginID: String
    private let manifest: PluginManifest
    private let workspace: WorkspaceFileSystem
    private let store: PluginStore
    private let permissions: Set<String>

    init(
        pluginID: String,
        manifest: PluginManifest,
        workspace: WorkspaceFileSystem,
        store: PluginStore
    ) {
        self.pluginID = pluginID
        self.manifest = manifest
        self.workspace = workspace
        self.store = store
        self.permissions = Set(manifest.permissions)
    }

    func luaPluginHostCall(
        method: String,
        argumentA: String?,
        argumentB: String?,
        argumentC: String?
    ) -> String? {
        do {
            switch method {
            case "workspace.list":
                try require("workspace.read")
                let path = argumentA ?? "."
                let depth = Int(argumentB ?? "2") ?? 2
                let entries = try workspace.listFiles(
                    path: path,
                    maxDepth: min(max(depth, 1), 6)
                )
                return try encode(entries)

            case "workspace.read":
                try require("workspace.read")
                guard let path = argumentA else {
                    return errorJSON("workspace.read requires a path")
                }
                return try workspace.readText(path: path)

            case "workspace.write":
                try require("workspace.write")
                guard let path = argumentA, let content = argumentB else {
                    return errorJSON("workspace.write requires path and content")
                }
                let overwrite = argumentC == "true"
                let result = try workspace.write(
                    path: path,
                    content: content,
                    overwrite: overwrite
                )
                return try encode(
                    PluginWriteResult(
                        path: result.path,
                        bytesWritten: result.bytesWritten,
                        created: result.created,
                        backupPath: result.backupPath
                    )
                )

            case "workspace.search":
                try require("workspace.read")
                guard let query = argumentA else {
                    return errorJSON("workspace.search requires a query")
                }
                let include = argumentB.flatMap { $0.isEmpty ? nil : $0 }
                let matches = try workspace.search(
                    query: query,
                    include: include,
                    maxResults: 100
                )
                return try encode(matches)

            case "workspace.root":
                try require("workspace.read")
                return workspace.root.path

            case "plugin.config":
                return try encode(manifestConfig())

            default:
                return errorJSON("Unknown host method: \(method)")
            }
        } catch {
            store.log(
                pluginID: pluginID,
                level: .error,
                message: "\(method): \(error.localizedDescription)"
            )
            return errorJSON(error.localizedDescription)
        }
    }

    func luaPluginLog(level: String, message: String) {
        let mapped: PluginLogEntry.Level
        switch level {
        case "debug": mapped = .debug
        case "warning": mapped = .warning
        case "error": mapped = .error
        default: mapped = .info
        }
        store.log(pluginID: pluginID, level: mapped, message: message)
    }

    private func manifestConfig() -> [String: JSONValue] {
        store.plugin(id: pluginID)?.config ?? [:]
    }

    private func require(_ permission: String) throws {
        guard permissions.contains(permission) else {
            throw WorkspaceError.fileSystem(
                "Plugin \(manifest.name) did not declare permission \(permission)."
            )
        }
    }

    private func encode<T: Encodable>(_ value: T) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(value)
        return String(decoding: data, as: UTF8.self)
    }

    private func errorJSON(_ message: String) -> String {
        let escaped = message
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
        return "{\"error\":\"\(escaped)\"}"
    }

    private struct PluginWriteResult: Encodable {
        let path: String
        let bytesWritten: Int
        let created: Bool
        let backupPath: String?
    }
}
