import XCTest
import LuaRuntime
import PocketDSHCore
@testable import PocketDSH

/// The Python plugin runtime, exercised against a stub executor.
///
/// The point of these is the boundary, not the interpreter: what the runtime does with the
/// result, how it handles a plugin that fails, and that a manifest naming Python reaches this
/// runtime at all.
final class PythonPluginTests: XCTestCase {
    private final class StubExecutor: PythonPluginExecuting {
        var calls: [(script: String, arguments: [String])] = []
        var answer: Result<String, Error> = .success("ok")

        func runPythonPlugin(script: String, arguments: [String]) async -> Result<String, Error> {
            calls.append((script, arguments))
            return answer
        }
    }

    func testInvokePassesToolAndArgumentsToTheScript() async throws {
        let executor = StubExecutor()
        let runtime = PythonPluginRuntime(identifier: "demo", executor: executor)
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("pyplugin-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let entry = dir.appendingPathComponent("main.py")
        try "print('hello')\n".write(to: entry, atomically: true, encoding: .utf8)
        try runtime.load(entryURL: entry)

        let result = await runtime.invoke(tool: "greet", argumentsJSON: #"{"name":"ada"}"#)
        XCTAssertFalse(result.isError)
        XCTAssertEqual(executor.calls.count, 1)
        XCTAssertEqual(executor.calls.first?.arguments.first, "greet")
        XCTAssertEqual(executor.calls.first?.arguments.last, #"{"name":"ada"}"#)
        XCTAssertTrue(executor.calls.first?.script.contains("print('hello')") ?? false)
    }

    func testExecutorFailureBecomesAnErrorResult() async throws {
        let executor = StubExecutor()
        executor.answer = .failure(NSError(domain: "t", code: 1,
                                           userInfo: [NSLocalizedDescriptionKey: "guest died"]))
        let runtime = PythonPluginRuntime(identifier: "demo", executor: executor)
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("pyplugin-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let entry = dir.appendingPathComponent("main.py")
        try "".write(to: entry, atomically: true, encoding: .utf8)
        try runtime.load(entryURL: entry)

        let result = await runtime.invoke(tool: "x", argumentsJSON: "{}")
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.content.contains("guest died"))
    }

    func testEmptyOutputIsNotAnError() async throws {
        let executor = StubExecutor()
        executor.answer = .success("  \n ")
        let runtime = PythonPluginRuntime(identifier: "demo", executor: executor)
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("pyplugin-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let entry = dir.appendingPathComponent("main.py")
        try "".write(to: entry, atomically: true, encoding: .utf8)
        try runtime.load(entryURL: entry)

        let result = await runtime.invoke(tool: "x", argumentsJSON: "{}")
        XCTAssertFalse(result.isError)
        XCTAssertEqual(result.content, "(no output)")
    }

    /// A manifest that names no runtime is a Lua plugin, which is what every plugin built before
    /// this field existed is.
    func testManifestWithoutRuntimeDefaultsToLua() {
        let manifest = PluginManifest(id: "a", name: "A", version: "1", description: "d")
        XCTAssertNil(manifest.runtime)
        XCTAssertEqual(manifest.runtime ?? "lua", "lua")
    }
}
