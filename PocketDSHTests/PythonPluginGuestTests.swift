import XCTest
@testable import PocketDSH

/// The Python plugin path through the real guest.
///
/// `PythonPluginTests` stubs the executor and covers the boundary; this covers what the stub
/// stands in for - a script written into the guest's writable /workspace, run by the guest's own
/// CPython, with the tool name and JSON arguments arriving as argv and the printed JSON coming
/// back as the result.
final class PythonPluginGuestTests: XCTestCase {

    @MainActor
    func testAPythonPluginRunsInTheGuest() async throws {
        let runtime = E2Runtime()
        guard let rootfs = runtime.resolvedRootfsPath else {
            throw XCTSkip("this build has no guest root")
        }
        let python = (rootfs as NSString).appendingPathComponent("usr/bin/python3.12")
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        // The same directory the bridge hands the guest as /workspace, asked for rather than
        // assumed: writing to TMPDIR instead put the script one level above where /workspace
        // points, and the guest reported the file missing.
        let workspace = URL(fileURLWithPath: runtime.resolvedWorkspacePath)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let script = workspace.appendingPathComponent("plugin-\(UUID().uuidString).py")
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import json, sys
        tool = sys.argv[1]
        args = json.loads(sys.argv[2])
        print(json.dumps({"tool": tool, "greeting": "hello " + args.get("name", "?")}))
        """.write(to: script, atomically: true, encoding: .utf8)

        let command = "python3.12 \"/workspace/\(script.lastPathComponent)\" "
            + "greet \"{\\\"name\\\":\\\"ada\\\"}\""
        let result = await runtime.run(command: command,
                                       workspace: script.deletingLastPathComponent())
        XCTAssertFalse(result.isError, result.content)
        XCTAssertTrue(result.content.contains("\"tool\": \"greet\""), result.content)
        XCTAssertTrue(result.content.contains("hello ada"), result.content)
    }
}
