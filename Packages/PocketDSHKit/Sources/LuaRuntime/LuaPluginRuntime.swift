import Foundation
import PocketDSHCore
import CLua

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

public struct LuaPluginToolResult: Sendable, Equatable {
    public let content: String
    public let isError: Bool

    public init(content: String, isError: Bool) {
        self.content = content
        self.isError = isError
    }
}

public enum LuaPluginRuntimeError: Error, LocalizedError {
    case couldNotCreateRuntime
    case loadFailed(String)
    case activateFailed(String)
    case encodingFailed

    public var errorDescription: String? {
        switch self {
        case .couldNotCreateRuntime:
            return "Could not create the plugin Lua runtime."
        case .loadFailed(let message):
            return "Could not load plugin: \(message)"
        case .activateFailed(let message):
            return "Could not activate plugin: \(message)"
        case .encodingFailed:
            return "Could not encode plugin data."
        }
    }
}

public protocol LuaPluginHost: AnyObject {
    func luaPluginHostCall(
        method: String,
        argumentA: String?,
        argumentB: String?,
        argumentC: String?
    ) -> String?

    func luaPluginLog(level: String, message: String)
}

private let luaPluginHostCallback: @convention(c) (
    UnsafeMutableRawPointer?,
    UnsafePointer<CChar>?,
    UnsafePointer<CChar>?,
    UnsafePointer<CChar>?,
    UnsafePointer<CChar>?
) -> UnsafeMutablePointer<CChar>? = { userdata, method, a, b, c in
    guard let userdata, let method else { return nil }
    let runtime = Unmanaged<LuaPluginRuntime>
        .fromOpaque(userdata)
        .takeUnretainedValue()
    let response = runtime.handleHostCall(
        method: String(cString: method),
        a: a.map { String(cString: $0) },
        b: b.map { String(cString: $0) },
        c: c.map { String(cString: $0) }
    )
    guard let response else { return nil }
    return strdup(response)
}

private let luaPluginLogCallback: @convention(c) (
    UnsafeMutableRawPointer?,
    Int32,
    UnsafePointer<CChar>?
) -> Void = { userdata, level, message in
    guard let userdata, let message else { return }
    let runtime = Unmanaged<LuaPluginRuntime>
        .fromOpaque(userdata)
        .takeUnretainedValue()
    let names = ["debug", "info", "warning", "error"]
    let index = max(0, min(Int(level), names.count - 1))
    runtime.host?.luaPluginLog(
        level: names[index],
        message: String(cString: message)
    )
}

/// A persistent Lua state for one installed plugin.
///
/// Plugins define `handle_tool(name, args)` and optionally `activate(config)`.
/// The host owns all workspace access; Lua never receives a raw filesystem API.
public final class LuaPluginRuntime: @unchecked Sendable {
    public let identifier: String
    public private(set) var host: LuaPluginHost?
    private var runtime: OpaquePointer?
    private let lock = NSLock()

    public init(identifier: String) {
        self.identifier = identifier
    }

    deinit {
        if let runtime {
            pocket_plugin_free(runtime)
        }
    }

    public func load(
        entryURL: URL,
        config: [String: JSONValue],
        host: LuaPluginHost
    ) throws {
        self.host = host
        let userdata = Unmanaged.passUnretained(self).toOpaque()

        guard let runtime = pocket_plugin_new(
            luaPluginHostCallback,
            luaPluginLogCallback,
            userdata
        ) else {
            throw LuaPluginRuntimeError.couldNotCreateRuntime
        }
        self.runtime = runtime

        let loadStatus = entryURL.path.withCString { path in
            pocket_plugin_load(runtime, path)
        }
        guard loadStatus == 0 else {
            throw LuaPluginRuntimeError.loadFailed(
                "Lua reported an error while loading \(entryURL.lastPathComponent); see plugin logs."
            )
        }

        let configLiteral = JSONValue.object(config).luaLiteral
        let activateStatus = configLiteral.withCString { literal in
            pocket_plugin_activate(runtime, literal)
        }
        guard activateStatus == 0 else {
            throw LuaPluginRuntimeError.activateFailed(
                "activate(config) failed; see plugin logs."
            )
        }
    }

    public func invoke(
        tool: String,
        arguments: [String: JSONValue]
    ) -> LuaPluginToolResult {
        lock.lock()
        let runtime = self.runtime
        lock.unlock()

        guard let runtime else {
            return LuaPluginToolResult(
                content: "Plugin runtime is not loaded.",
                isError: true
            )
        }

        let argumentsLiteral = JSONValue.object(arguments).luaLiteral
        let resultPointer: UnsafeMutablePointer<CChar>? = tool.withCString { toolPointer in
            argumentsLiteral.withCString { argumentPointer in
                pocket_plugin_invoke(runtime, toolPointer, argumentPointer)
            }
        }

        guard let resultPointer else {
            return LuaPluginToolResult(
                content: "Plugin runtime returned no result.",
                isError: true
            )
        }
        defer { pocket_plugin_free_string(resultPointer) }

        let resultString = String(cString: resultPointer)
        guard let data = resultString.data(using: .utf8),
              let decoded = try? JSONDecoder().decode(PluginInvocationEnvelope.self, from: data) else {
            return LuaPluginToolResult(content: resultString, isError: false)
        }
        return LuaPluginToolResult(
            content: decoded.content,
            isError: decoded.isError
        )
    }

    public func cancel() {
        lock.lock()
        let runtime = self.runtime
        lock.unlock()
        if let runtime {
            pocket_plugin_cancel(runtime)
        }
    }

    fileprivate func handleHostCall(
        method: String,
        a: String?,
        b: String?,
        c: String?
    ) -> String? {
        host?.luaPluginHostCall(
            method: method,
            argumentA: a,
            argumentB: b,
            argumentC: c
        )
    }

    private struct PluginInvocationEnvelope: Decodable {
        let content: String
        let isError: Bool

        enum CodingKeys: String, CodingKey {
            case content
            case isError = "is_error"
        }
    }
}

private extension JSONValue {
    var luaLiteral: String {
        switch self {
        case .null:
            return "nil"
        case .bool(let value):
            return value ? "true" : "false"
        case .number(let value):
            if value.rounded() == value, abs(value) < Double(Int.max) {
                return String(Int(value))
            }
            return String(value)
        case .string(let value):
            return "\"\(escape(value))\""
        case .array(let values):
            return "{" + values.map(\.luaLiteral).joined(separator: ",") + "}"
        case .object(let values):
            let entries = values
                .sorted(by: { $0.key < $1.key })
                .map { "[\"\(escape($0.key))\"]=" + $0.value.luaLiteral }
                .joined(separator: ",")
            return "{" + entries + "}"
        }
    }

    func escape(_ value: String) -> String {
        var output = ""
        for scalar in value.unicodeScalars {
            switch scalar {
            case "\"": output += "\\\""
            case "\\": output += "\\\\"
            case "\n": output += "\\n"
            case "\r": output += "\\r"
            case "\t": output += "\\t"
            default:
                if scalar.value < 32 {
                    output += String(format: "\\%03d", scalar.value)
                } else {
                    output.unicodeScalars.append(scalar)
                }
            }
        }
        return output
    }
}
