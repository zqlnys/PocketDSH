import Combine
import Foundation
import PocketDSHCore

final class PluginStore: ObservableObject {
    @Published private(set) var plugins: [InstalledPlugin] = []

    /// Plugin directories that exist but could not be loaded, with the reason.
    ///
    /// They used to be skipped in silence: an installed plugin whose manifest was corrupt simply
    /// vanished from the list, and the directory stayed on disk with nothing naming it.
    @Published private(set) var loadFailures: [String] = []
    @Published private(set) var logs: [PluginLogEntry] = []

    private let fileManager = FileManager.default
    private let root: URL
    private let stateURL: URL
    private let logsURL: URL

    init(root customRoot: URL? = nil) {
        if let customRoot {
            root = customRoot.standardizedFileURL
        } else {
            let support = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
                ?? fileManager.temporaryDirectory
            root = support
                .appendingPathComponent("PocketDSH", isDirectory: true)
                .appendingPathComponent("Plugins", isDirectory: true)
        }
        stateURL = root.appendingPathComponent("plugin-state.json")
        logsURL = root.appendingPathComponent("plugin-logs.json")
        try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        load()
    }

    func plugin(id: String) -> InstalledPlugin? {
        plugins.first { $0.manifest.id == id }
    }

    func directory(for id: String) -> URL {
        root.appendingPathComponent(id, isDirectory: true)
    }

    func entryURL(for plugin: InstalledPlugin) -> URL {
        directory(for: plugin.manifest.id).appendingPathComponent(plugin.manifest.entry)
    }

    @discardableResult
    func install(from sourceURL: URL) throws -> InstalledPlugin {
        let didAccess = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if didAccess { sourceURL.stopAccessingSecurityScopedResource() }
        }

        let manifestURL = sourceURL.appendingPathComponent("plugin.json")
        let manifest: PluginManifest
        do {
            let data = try Data(contentsOf: manifestURL)
            manifest = try JSONDecoder().decode(PluginManifest.self, from: data)
            try manifest.validate()
        } catch {
            throw WorkspaceError.fileSystem("Invalid plugin.json: \(error.localizedDescription)")
        }

        guard self.plugin(id: manifest.id) == nil else {
            throw WorkspaceError.fileSystem("Plugin \(manifest.id) is already installed.")
        }
        let entry = sourceURL.appendingPathComponent(manifest.entry)
        guard fileManager.fileExists(atPath: entry.path) else {
            throw WorkspaceError.notFound(manifest.entry)
        }

        let destination = directory(for: manifest.id)
        do {
            try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
            try fileManager.copyItem(at: sourceURL, to: destination)
        } catch {
            throw WorkspaceError.fileSystem("Could not install plugin: \(error.localizedDescription)")
        }

        let installed = InstalledPlugin(
            manifest: manifest,
            enabled: false,
            configValues: defaultConfig(for: manifest)
        )
        plugins.append(installed)
        sortPlugins()
        saveState()
        log(pluginID: manifest.id, level: .info, message: "Installed v\(manifest.version)")
        return installed
    }

    @discardableResult
    func installExample() throws -> InstalledPlugin {
        let manifest: PluginManifest
        do {
            manifest = try JSONDecoder().decode(
                PluginManifest.self,
                from: Data(EmbeddedExamplePlugin.manifestJSON.utf8)
            )
            try manifest.validate()
        } catch {
            throw WorkspaceError.fileSystem("Embedded example plugin is invalid: \(error.localizedDescription)")
        }

        guard plugin(id: manifest.id) == nil else {
            throw WorkspaceError.fileSystem("Example plugin is already installed.")
        }

        let destination = directory(for: manifest.id)
        do {
            try fileManager.createDirectory(at: destination, withIntermediateDirectories: true)
            try EmbeddedExamplePlugin.manifestJSON.write(
                to: destination.appendingPathComponent("plugin.json"),
                atomically: true,
                encoding: .utf8
            )
            try EmbeddedExamplePlugin.luaSource.write(
                to: destination.appendingPathComponent("main.lua"),
                atomically: true,
                encoding: .utf8
            )
            try EmbeddedExamplePlugin.readme.write(
                to: destination.appendingPathComponent("README.txt"),
                atomically: true,
                encoding: .utf8
            )
        } catch {
            throw WorkspaceError.fileSystem("Could not write example plugin: \(error.localizedDescription)")
        }

        let installed = InstalledPlugin(
            manifest: manifest,
            enabled: true,
            configValues: defaultConfig(for: manifest)
        )
        plugins.append(installed)
        sortPlugins()
        saveState()
        log(pluginID: manifest.id, level: .info, message: "Installed example v\(manifest.version)")
        return installed
    }

    func setEnabled(_ enabled: Bool, for id: String) {
        guard let index = plugins.firstIndex(where: { $0.manifest.id == id }) else { return }
        plugins[index].enabled = enabled
        plugins[index].lastError = nil
        saveState()
        log(pluginID: id, level: .info, message: enabled ? "Enabled" : "Disabled")
    }

    func updateConfig(_ values: [String: JSONValue], for id: String) {
        guard let index = plugins.firstIndex(where: { $0.manifest.id == id }) else { return }
        plugins[index].configValues = values
        saveState()
        log(pluginID: id, level: .info, message: "Configuration saved")
    }

    func setError(_ message: String, for id: String) {
        guard let index = plugins.firstIndex(where: { $0.manifest.id == id }) else { return }
        plugins[index].lastError = message
        saveState()
        log(pluginID: id, level: .error, message: message)
    }

    func clearError(for id: String) {
        guard let index = plugins.firstIndex(where: { $0.manifest.id == id }) else { return }
        plugins[index].lastError = nil
        saveState()
    }

    func uninstall(_ plugin: InstalledPlugin) {
        let id = plugin.manifest.id
        try? fileManager.removeItem(at: directory(for: id))
        plugins.removeAll { $0.manifest.id == id }
        logs.removeAll { $0.pluginID == id }
        saveState()
        saveLogs()
    }

    func log(pluginID: String, level: PluginLogEntry.Level, message: String) {
        logs.append(
            PluginLogEntry(
                pluginID: pluginID,
                level: level,
                message: message
            )
        )
        if logs.count > 800 {
            logs.removeFirst(logs.count - 800)
        }
        saveLogs()
    }

    func logs(for id: String) -> [PluginLogEntry] {
        logs.filter { $0.pluginID == id }
    }

    private func defaultConfig(for manifest: PluginManifest) -> [String: JSONValue] {
        var values: [String: JSONValue] = [:]
        for field in manifest.config {
            if let defaultValue = field.defaultValue {
                values[field.key] = defaultValue
            }
        }
        return values
    }

    private func sortPlugins() {
        plugins.sort { $0.manifest.name.localizedStandardCompare($1.manifest.name) == .orderedAscending }
    }

    private func load() {
        try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        var stored: [String: StoredState] = [:]
        if let data = try? Data(contentsOf: stateURL),
           let decoded = try? JSONDecoder().decode([String: StoredState].self, from: data) {
            stored = decoded
        }

        let contents = (try? fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        )) ?? []

        var loaded: [InstalledPlugin] = []
        var unreadable: [String] = []
        for directory in contents {
            let manifestURL = directory.appendingPathComponent("plugin.json")
            // A plugin directory that cannot be read is not an absent plugin.
            //
            // These used to be skipped in silence, so an installed plugin whose manifest was
            // corrupt vanished from the list with nothing to say what it had been - and the
            // directory was still on disk the whole time.
            guard let data = try? Data(contentsOf: manifestURL) else {
                unreadable.append("\(directory.lastPathComponent): plugin.json could not be read")
                continue
            }
            guard let manifest = try? JSONDecoder().decode(PluginManifest.self, from: data) else {
                unreadable.append("\(directory.lastPathComponent): plugin.json is not a valid manifest")
                continue
            }
            do {
                try manifest.validate()
            } catch {
                unreadable.append("\(directory.lastPathComponent): \(error.localizedDescription)")
                continue
            }
            let state = stored[manifest.id]
            loaded.append(
                InstalledPlugin(
                    manifest: manifest,
                    enabled: state?.enabled ?? false,
                    configValues: state?.configValues ?? defaultConfig(for: manifest),
                    installedAt: state?.installedAt ?? .now,
                    lastError: state?.lastError
                )
            )
        }
        plugins = loaded.sorted {
            $0.manifest.name.localizedStandardCompare($1.manifest.name) == .orderedAscending
        }

        // Kept where a settings screen can show them, so a plugin that failed to load can be named.
        loadFailures = unreadable

        if let data = try? Data(contentsOf: logsURL) {
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601
            if let decoded = try? decoder.decode([PluginLogEntry].self, from: data) {
                logs = decoded
            }
        }
    }

    private func saveState() {
        var values: [String: StoredState] = [:]
        for plugin in plugins {
            values[plugin.manifest.id] = StoredState(
                enabled: plugin.enabled,
                configValues: plugin.configValues,
                installedAt: plugin.installedAt,
                lastError: plugin.lastError
            )
        }
        do {
            try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(values)
            try data.write(to: stateURL, options: .atomic)
        } catch {
            // Keep in-memory state; next save retries.
        }
    }

    private func saveLogs() {
        do {
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(logs)
            try data.write(to: logsURL, options: .atomic)
        } catch {
            // Logs are diagnostic; don't fail the plugin operation.
        }
    }

    private struct StoredState: Codable {
        var enabled: Bool
        var configValues: [String: JSONValue]
        var installedAt: Date
        var lastError: String?
    }
}

enum EmbeddedExamplePlugin {
    static let manifestJSON = """
    {
      "id": "com.example.pocketdsh.workspace-inspector",
      "name": "Workspace Inspector",
      "version": "1.0.0",
      "description": "A real local plugin: lists the iPhone workspace and writes a note through the guarded host API.",
      "author": "PocketDSH",
      "apiVersion": 1,
      "entry": "main.lua",
      "permissions": ["workspace.read", "workspace.write"],
      "tools": [
        {
          "name": "workspace_report",
          "description": "List real files in the current iPhone workspace.",
          "parameters": {
            "type": "object",
            "properties": {
              "path": { "type": "string", "description": "Relative directory, default ." },
              "depth": { "type": "integer", "description": "Recursive depth, default 2" }
            },
            "required": [],
            "additionalProperties": false
          }
        },
        {
          "name": "write_pocket_note",
          "description": "Write a real Markdown note in the current workspace. Overwrites the target through the host backup path.",
          "parameters": {
            "type": "object",
            "properties": {
              "message": { "type": "string", "description": "Note body" },
              "path": { "type": "string", "description": "Target path, default POCKET_NOTE.md" }
            },
            "required": ["message"],
            "additionalProperties": false
          }
        }
      ],
      "config": [
        {
          "key": "prefix",
          "title": "Note prefix",
          "description": "Heading inserted before the note body.",
          "type": "string",
          "defaultValue": "Pocket note"
        }
      ]
    }
    """

    static let luaSource = """
    local config = {}

    function activate(values)
      config = values or {}
      plugin.log("info", "Workspace Inspector activated")
    end

    function handle_tool(name, args)
      if name == "workspace_report" then
        local path = args.path or "."
        local depth = tonumber(args.depth) or 2
        local listing = plugin.workspace_list(path, depth)
        if listing == nil then
          return plugin.result("Workspace listing failed", true)
        end
        return plugin.result("Real workspace listing for " .. path .. ":\\n" .. listing, false)
      end

      if name == "write_pocket_note" then
        local message = args.message or ""
        local path = args.path or "POCKET_NOTE.md"
        local prefix = config.prefix or "Pocket note"
        local body = "# " .. prefix .. "\\n\\n" .. message .. "\\n"
        local result = plugin.workspace_write(path, body, true)
        if result == nil then
          return plugin.result("Could not write " .. path, true)
        end
        plugin.log("info", "Wrote " .. path)
        return plugin.result("Real file written: " .. path .. "\\n" .. result, false)
      end

      return plugin.result("Unknown tool: " .. name, true)
    end
    """

    static let readme = """
    Workspace Inspector — PocketDSH example plugin

    Tools:
    - workspace_report: lists the current iPhone workspace.
    - write_pocket_note: writes a Markdown note through the guarded host API.

    Permissions:
    - workspace.read
    - workspace.write

    The plugin runs in the embedded Lua 5.4 runtime. It has no io/os/shell access;
    all workspace operations go through PocketDSH's path guard and backup system.
    """
}
