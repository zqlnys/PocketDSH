import Foundation
import XCTest
import PocketDSHCore
@testable import LuaRuntime

private final class MockPluginHost: LuaPluginHost {
    var logs: [(String, String)] = []
    var calls: [(String, String?, String?, String?)] = []
    var workspaceWriteResult = "{\"ok\":true}"

    func luaPluginHostCall(
        method: String,
        argumentA: String?,
        argumentB: String?,
        argumentC: String?
    ) -> String? {
        calls.append((method, argumentA, argumentB, argumentC))
        if method == "workspace.write" {
            return workspaceWriteResult
        }
        if method == "workspace.read" {
            return "file contents"
        }
        if method == "plugin.config" {
            return "{\"greeting\":\"hi\"}"
        }
        return "{}"
    }

    func luaPluginLog(level: String, message: String) {
        logs.append((level, message))
    }
}

final class LuaPluginRuntimeTests: XCTestCase {
    private var directory: URL!

    override func setUpWithError() throws {
        try super.setUpWithError()
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHPluginTests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let directory { try? FileManager.default.removeItem(at: directory) }
        try super.tearDownWithError()
    }

    private func writePlugin(_ source: String) throws -> URL {
        let url = directory.appendingPathComponent("main.lua")
        try source.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    func testPluginLoadsActivatesAndInvokesTool() throws {
        let entry = try writePlugin(
            """
            local greeting = "default"
            function activate(config)
              greeting = config.greeting or greeting
              plugin.log("info", "activated with " .. greeting)
            end

            function handle_tool(name, args)
              if name == "echo" then
                return plugin.result(greeting .. ":" .. (args.text or "none"), false)
              elseif name == "write" then
                local result = plugin.workspace_write("note.txt", "hello from plugin", true)
                return plugin.result(result or "nil", false)
              elseif name == "fail" then
                return plugin.result("intentional plugin failure", true)
              end
              return plugin.result("unknown tool", true)
            end
            """
        )

        let host = MockPluginHost()
        let runtime = LuaPluginRuntime(identifier: "com.example.test")
        try runtime.load(
            entryURL: entry,
            config: ["greeting": .string("hello")],
            host: host
        )

        let echo = runtime.invoke(
            tool: "echo",
            arguments: ["text": .string("world")]
        )
        XCTAssertFalse(echo.isError)
        XCTAssertEqual(echo.content, "hello:world")
        XCTAssertTrue(host.logs.contains { $0.0 == "info" && $0.1.contains("hello") })

        let write = runtime.invoke(
            tool: "write",
            arguments: [:]
        )
        XCTAssertFalse(write.isError)
        XCTAssertEqual(write.content, "{\"ok\":true}")
        XCTAssertTrue(host.calls.contains {
            $0.0 == "workspace.write" && $0.1 == "note.txt" && $0.2 == "hello from plugin" && $0.3 == "true"
        })

        let failure = runtime.invoke(tool: "fail", arguments: [:])
        XCTAssertTrue(failure.isError)
        XCTAssertEqual(failure.content, "intentional plugin failure")
    }

    func testPluginSyntaxErrorFailsWithLog() throws {
        let entry = try writePlugin("function handle_tool( this is not lua")
        let host = MockPluginHost()
        let runtime = LuaPluginRuntime(identifier: "com.example.broken")

        XCTAssertThrowsError(
            try runtime.load(entryURL: entry, config: [:], host: host)
        )
        XCTAssertTrue(host.logs.contains { $0.0 == "error" })
    }

    func testPluginWithoutHandlerReturnsError() throws {
        let entry = try writePlugin("local value = 1")
        let host = MockPluginHost()
        let runtime = LuaPluginRuntime(identifier: "com.example.empty")
        try runtime.load(entryURL: entry, config: [:], host: host)

        let result = runtime.invoke(tool: "echo", arguments: [:])
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.content.contains("handle_tool"))
    }
}
