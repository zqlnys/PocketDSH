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

    /// A run that cannot be started leaves the process able to try again.
    ///
    /// The redirect restored stdout only on the path that succeeded. When the first dup2 worked and
    /// the second failed, the function returned with stdout still pointing at a file it then
    /// closed - so every later write in the process went to a closed descriptor. One failed run
    /// silently broke the ones after it.
    @MainActor
    func testFailedStartDoesNotPoisonTheNextRun() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("bridge-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }

        let executor = makeExecutor(root: project, tier: .tierD)
        // A run whose output file cannot be created: the tool reports a failure.
        let failed = await executor.execute(
            name: ToolNames.runLinux,
            argumentsJSON: #"{"command":"true"}"#
        )
        XCTAssertFalse(failed.content.isEmpty)

        // The next run must still be able to produce output.
        let after = await executor.execute(
            name: ToolNames.runLinux,
            argumentsJSON: #"{"command":"echo still-works"}"#
        )
        XCTAssertTrue(after.content.contains("still-works"), after.content)
    }

    /// The guest starts in a UTF-8 locale, which is what CPython needs to read its own arguments.
    ///
    /// Under LANG=C the interpreter truncates argv to a few characters - `/workspace/a.py` becomes
    /// `/wor` - and the bridge sets both LANG and LC_ALL for that reason. LC_ALL is the one that
    /// wins for every category, so a script exporting LANG=C for its own children cannot drag
    /// Python's decoding into the broken path. This checks the environment a guest actually gets.
    @MainActor
    func testGuestStartsWithAUTF8Locale() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("locale-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }

        let executor = makeExecutor(root: project, tier: .tierD)
        let result = await executor.execute(name: ToolNames.runLinux,
                                            argumentsJSON: #"{"command":"env"}"#)
        XCTAssertFalse(result.isError, result.content)
        XCTAssertTrue(result.content.contains("LC_ALL=C.UTF-8"),
                      "the guest did not inherit the UTF-8 locale: \(result.content)")
    }

    /// The tool refuses a command it was not given, rather than running something arbitrary.
    @MainActor
    func testRunLinuxRequiresACommand() async throws {
        let executor = makeExecutor(root: FileManager.default.temporaryDirectory, tier: .tierD)
        let result = await executor.execute(name: ToolNames.runLinux, argumentsJSON: "{}")
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.content.contains("requires a non-empty command"), result.content)

        // Whitespace is not a command either: it used to run and report success with no output.
        for blank in ["   ", "\n\t "] {
            let spaces = await executor.execute(name: ToolNames.runLinux,
                                                argumentsJSON: "{\"command\":\"\(blank)\"}")
            XCTAssertTrue(spaces.isError, "whitespace ran as a command: \(spaces.content)")
        }
    }
}
