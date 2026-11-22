import XCTest
@testable import PocketDSH

/// epoll, which is what Python's selectors prefers and what asyncio therefore asks for.
///
/// Before this existed selectors fell back to PollSelector: it worked, so nothing looked broken,
/// and the fallback hid the gap. The assertion that matters is not that a selector works but that
/// it is the one the program asked for.
final class EpollTests: XCTestCase {

    @MainActor
    func testSelectorsChoosesEpoll() async throws {
        let runtime = E2Runtime()
        guard let rootfs = runtime.resolvedRootfsPath else { throw XCTSkip("no guest root") }
        let python = (rootfs as NSString).appendingPathComponent("usr/bin/python3.12")
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python), "no CPython")

        let script = URL(fileURLWithPath: runtime.resolvedWorkspacePath)
            .appendingPathComponent("epoll-\(UUID().uuidString).py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import selectors, os
        s = selectors.DefaultSelector()
        print("selector", type(s).__name__)
        r, w = os.pipe()
        s.register(r, selectors.EVENT_READ, data=7)
        print("empty", s.select(0))
        os.write(w, b"x")
        for key, mask in s.select(0.5):
            print("ready", key.fd == r, key.data, bool(mask & selectors.EVENT_READ))
        s.close()
        """.write(to: script, atomically: true, encoding: .utf8)

        let result = await runtime.run(command: "python3.12 /workspace/\(script.lastPathComponent)")
        XCTAssertFalse(result.isError, result.content)
        XCTAssertTrue(result.content.contains("selector EpollSelector"),
                      "fell back: \(result.content)")
        XCTAssertTrue(result.content.contains("ready True 7 True"), result.content)
    }
}
