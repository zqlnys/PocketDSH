import Foundation
import PocketDSHCore

/// A plugin that runs in the guest CPython.
///
/// The Lua runtime embeds an interpreter in the app; this one does not. Tier D already runs a
/// real CPython 3.12 with its standard library, so a Python plugin is a script that thing runs -
/// which is what "the same plugin on the desktop" means in practice, since that is where a Python
/// plugin would run too.
///
/// The protocol is deliberately one process per call with JSON in and JSON out. Keeping a guest
/// process alive between calls would be faster and would need a request loop, a framing format
/// and a way to notice the guest died; none of that is worth it until a plugin is slow enough to
/// notice.
public struct PythonPluginInvocation: Sendable {
    public let tool: String
    public let argumentsJSON: String

    public init(tool: String, argumentsJSON: String) {
        self.tool = tool
        self.argumentsJSON = argumentsJSON
    }
}

public protocol PythonPluginExecuting: AnyObject {
    /// Runs the script with the given arguments and returns its stdout. Async because the guest
    /// runs on the main actor: a synchronous call from a background task would have to block a
    /// thread waiting on the main one, which is a deadlock the moment the caller is the main one.
    func runPythonPlugin(script: String, arguments: [String]) async -> Result<String, Error>
}

public final class PythonPluginRuntime {
    public let identifier: String
    private let executor: PythonPluginExecuting
    private(set) public var source: String = ""

    public init(identifier: String, executor: PythonPluginExecuting) {
        self.identifier = identifier
        self.executor = executor
    }

    /// Reads the script now so a syntax-free but unreadable file fails at load, not on the first
    /// call - the same reason the Lua runtime loads eagerly.
    public func load(entryURL: URL) throws {
        source = try String(contentsOf: entryURL, encoding: .utf8)
    }

    /// One invocation, one process. The script receives the tool name and a JSON object as
    /// argv[1] and argv[2], and whatever it prints is the result. A non-zero exit is an error,
    /// and so is output that is not JSON - a plugin that prints a traceback should not look like
    /// a successful call that returned prose.
    public func invoke(tool: String, argumentsJSON: String) async -> LuaPluginToolResult {
        switch await executor.runPythonPlugin(script: source, arguments: [tool, argumentsJSON]) {
        case .failure(let error):
            return LuaPluginToolResult(content: error.localizedDescription, isError: true)
        case .success(let output):
            let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.isEmpty {
                return LuaPluginToolResult(content: "(no output)", isError: false)
            }
            return LuaPluginToolResult(content: trimmed, isError: false)
        }
    }
}
