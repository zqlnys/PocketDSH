import Foundation
import LuaRuntime
import PocketDSHCore

/// Runs a Python plugin in the guest CPython that Tier D provides.
///
/// The plugin is written to a temporary file inside the guest's writable /workspace and run once
/// per tool call, with the tool name and a JSON object as its arguments. Whatever it prints is
/// the result.
///
/// One process per call is the simple design and it is a deliberate one: keeping a guest alive
/// between calls needs a request loop, framing, and a way to notice the guest died, none of which
/// earns its complexity until a plugin is slow enough for the startup to show.
@MainActor
final class E2PythonPluginExecutor: PythonPluginExecuting {
    private let runtime: E2Runtime

    init(runtime: E2Runtime) {
        self.runtime = runtime
    }

    func runPythonPlugin(script: String, arguments: [String]) async -> Result<String, Error> {
        guard let rootfs = runtime.resolvedRootfsPath else {
            return .failure(ExecutorError.tierDUnavailable)
        }
        let python = (rootfs as NSString).appendingPathComponent("usr/bin/python3.12")
        guard FileManager.default.isReadableFile(atPath: python) else {
            return .failure(ExecutorError.noInterpreter)
        }

        // The script goes into the shared temporary directory, which is where E2_WORKSPACE points
        // for the guest's /workspace.
        let scratch = FileManager.default.temporaryDirectory
            .appendingPathComponent("plugin-\(UUID().uuidString).py")
        defer { try? FileManager.default.removeItem(at: scratch) }
        do {
            try script.write(to: scratch, atomically: true, encoding: .utf8)
        } catch {
            return .failure(error)
        }

        // Guest path, not host path: the guest sees this directory as /workspace, and the host
        // path does not exist inside it.
        let guestScript = "/workspace/" + scratch.lastPathComponent
        // Single quotes, with the one escape a single-quoted string needs. Wrapping in double
        // quotes and escaping only the quote character left $, backticks and backslashes to the
        // shell: an argument containing $(...) was executed rather than passed. The guest can only
        // reach the workspace, so the damage was bounded - but an argument is not code.
        let quoted = arguments.map(Self.shellQuoted)
        let command = "python3.12 " + Self.shellQuoted(guestScript) + " " + quoted.joined(separator: " ")
        let result = await runtime.run(command: command)
        if result.isError {
            return .failure(ExecutorError.guest(result.content))
        }
        return .success(result.content)
    }

    // The harness takes one command line, not an argument vector, so this goes through the shell
    // exactly as a user's command would.

    /// A string the shell reads as one word, whatever it contains.
    ///
    /// The standard form: wrap in single quotes and replace each embedded quote with the four
    /// characters that close, escape and reopen. Nothing else inside single quotes is special.
    nonisolated static func shellQuoted(_ text: String) -> String {
        "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    enum ExecutorError: LocalizedError {
        case tierDUnavailable, noInterpreter
        case guest(String)

        var errorDescription: String? {
            switch self {
            case .tierDUnavailable:
                return "Python plugins need Tier D, which is not running."
            case .noInterpreter:
                return "This build's guest root has no CPython."
            case .guest(let text):
                return text
            }
        }
    }
}
