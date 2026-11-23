import XCTest
@testable import PocketDSH

/// The `os` surface a program actually uses, exercised in the guest.
///
/// Not a differential test: qemu is not chrooted, so a path means the host's file in one and the
/// guest root's in the other, and comparing them compares two directories. It is a smoke test -
/// each operation must return something sensible, and the ones that must fail must fail.
///
/// This exists because `time.sleep` was a no-op for the whole life of the backend and nothing
/// noticed: no test had ever called it. The same is true of everything here.
final class OsSurfaceTests: XCTestCase {

    @MainActor
    func testTheOsSurfaceWorksInTheGuest() async throws {
        let root = Bundle.main.url(forResource: "alpine-root", withExtension: nil)
            ?? URL(fileURLWithPath: "/nonexistent")
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        let workspace = URL(fileURLWithPath: E2Runtime().resolvedWorkspacePath)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let script = workspace.appendingPathComponent("os-\(UUID().uuidString).py")
        defer { try? FileManager.default.removeItem(at: script) }
        // The directory is name-spaced and removed at the end, so a rerun starts clean.
        try """
        import os, shutil, stat, uuid

        d = "/workspace/osprobe-" + uuid.uuid4().hex[:8]
        os.mkdir(d)
        with open(d + "/f.txt", "w") as h:
            h.write("hello")

        def check(name, ok):
            print(("ok " if ok else "NO ") + name)

        check("listdir", os.listdir(d) == ["f.txt"])
        check("stat-size", os.stat(d + "/f.txt").st_size == 5)
        check("isreg", stat.S_ISREG(os.stat(d + "/f.txt").st_mode))
        check("isdir", stat.S_ISDIR(os.stat(d).st_mode))
        check("access", os.access(d + "/f.txt", os.R_OK))

        os.rename(d + "/f.txt", d + "/g.txt")
        check("rename", os.listdir(d) == ["g.txt"])
        os.remove(d + "/g.txt")
        check("remove", os.listdir(d) == [])

        os.symlink("target", d + "/lnk")
        check("readlink", os.readlink(d + "/lnk") == "target")
        os.chmod(d, 0o755)
        check("chmod", (os.stat(d).st_mode & 0o777) == 0o755)

        os.makedirs(d + "/a/b")
        check("makedirs", os.path.isdir(d + "/a/b"))
        check("getcwd-is-absolute", os.getcwd().startswith("/"))
        check("uname", os.uname().sysname == "Linux")

        try:
            open(d + "/missing/x", "w")
            check("missing-parent-fails", False)
        except FileNotFoundError:
            check("missing-parent-fails", True)

        shutil.rmtree(d)
        check("cleanup", not os.path.exists(d))
        """.write(to: script, atomically: true, encoding: .utf8)

        let runtime = E2Runtime()
        let result = await runtime.run(command: "python3.12 /workspace/\(script.lastPathComponent)",
                                       workspace: workspace)
        XCTAssertFalse(result.isError, result.content)
        let failures = result.content.split(separator: "\n")
            .filter { $0.hasPrefix("NO ") }
            .map(String.init)
        let joined = failures.joined(separator: "; ")
        XCTAssertTrue(failures.isEmpty, "failed checks: \(joined) -- \(result.content)")
        XCTAssertTrue(result.content.contains("ok cleanup"), result.content)
    }
}
