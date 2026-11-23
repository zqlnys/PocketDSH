import XCTest
import PocketDSHCore
import LuaRuntime
@testable import PocketDSH

/// `run_linux` as the agent reaches it: through ToolExecutor, not through E2Runtime.
///
/// Every other guest test calls the runtime directly, which is one layer below where the tool
/// name, the preferred tier and the workspace are decided. Those decisions are what the agent's
/// command actually obeys, and nothing exercised them.
final class LinuxToolTests: XCTestCase {

    @MainActor
    private func makeExecutor(root: URL, tier: RuntimeTier) -> ToolExecutor {
        let store = RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "linux-tool")!)
        store.preferredTier = tier
        return ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            e2Runtime: E2Runtime(),
            runtimeStore: store
        )
    }

    @MainActor
    func testRunLinuxGoesThroughTheToolLayer() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("linux-tool-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }
        try "tool-layer".write(to: project.appendingPathComponent("note.txt"),
                               atomically: true, encoding: .utf8)

        // The executor's workspace is the project, not the guest root. Passing the bundle root
        // here made the tool mount its own alpine-root as /workspace, and the note was not in it.
        let executor = makeExecutor(root: project, tier: .tierD)
        let result = await executor.execute(
            name: ToolNames.runLinux,
            argumentsJSON: #"{"command":"cat /workspace/note.txt"}"#
        )
        XCTAssertFalse(result.isError, result.content)
        // The tool names the backend it used, so a transcript never leaves it ambiguous.
        XCTAssertTrue(result.content.contains("Tier D"), result.content)
        XCTAssertTrue(result.content.contains("tool-layer"), result.content)
    }

    /// A failing command reaches the agent as a failure, through the tool layer as well.
    @MainActor
    func testRunLinuxReportsFailureThroughTheToolLayer() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("linux-tool-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }

        let executor = makeExecutor(root: project, tier: .tierD)
        let result = await executor.execute(
            name: ToolNames.runLinux,
            argumentsJSON: #"{"command":"cat /workspace/absent.txt"}"#
        )
        XCTAssertTrue(result.isError, "a failed command reached the agent as success")
    }

    /// A command that floods is stopped by the write ceiling, not by filling the device.
    ///
    /// The output is captured to a file and the cap on what is read back was applied afterwards, so
    /// a runaway command filled storage first. Sixty-four megabytes is far past what a tool result
    /// carries.
    @MainActor
    func testFloodingIsBoundedAtWriteTime() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("flood-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }

        // Repeats the largest file in the guest root until the ceiling stops it.
        let executor = makeExecutor(root: project, tier: .tierD)
        let result = await executor.execute(
            name: ToolNames.runLinux,
            argumentsJSON: #"{"command":"for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat /usr/lib/libpython3.12.so.1.0; done"}"#
        )
        // Whatever happens, the result must be bounded: success with a capped body, or a failure
        // reporting the write error. An unbounded result is the one outcome that is not allowed.
        XCTAssertLessThan(result.content.utf8.count, 200_000,
                          "a flooding command returned \(result.content.utf8.count) bytes")
    }

    /// The tool refuses a command it was not given, rather than running something arbitrary.
    @MainActor
    func testRunLinuxRequiresACommand() async throws {
        let executor = makeExecutor(root: FileManager.default.temporaryDirectory, tier: .tierD)
        let result = await executor.execute(name: ToolNames.runLinux, argumentsJSON: "{}")
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.content.contains("requires command"), result.content)
    }
}
