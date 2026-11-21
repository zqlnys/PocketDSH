import XCTest
@testable import PocketDSH

/// Proves Tier D actually executes a guest program on iOS.
///
/// The interpreter being linked into the app is not evidence that it runs: for several rounds
/// it compiled and bundled and had never once been executed. These tests run the bundled
/// busybox through the same bridge the app uses, so a regression shows up as a failing test
/// rather than as a Settings screen that silently reports failure.
final class E2BackendTests: XCTestCase {

    /// The guest root the app ships. Resolved from the test host's bundle, which is the app.
    private func guestRoot() throws -> URL {
        let bundle = Bundle(for: type(of: self))
        let candidates = [bundle, Bundle.main]
        for candidate in candidates {
            if let url = candidate.url(forResource: "alpine-root", withExtension: nil) {
                return url
            }
        }
        throw XCTSkip("alpine-root is not in the bundle; Tier D is not built into this target.")
    }

    /// Runs the harness the way a shell would: its own name first, then the program.
    ///
    /// The first version omitted the harness's own argv[0], so the busybox path landed at
    /// index 0 and the program's own name at index 1 - and the harness dutifully tried to
    /// load a file called "sh".
    private func runGuest(_ program: String, _ arguments: [String]) throws -> (status: Int32, output: String) {
        let argv = ["e2", program] + arguments
        let root = try guestRoot()
        let out = FileManager.default.temporaryDirectory
            .appendingPathComponent("e2-test-\(UUID().uuidString).out")
        defer { try? FileManager.default.removeItem(at: out) }

        var pointers: [UnsafeMutablePointer<CChar>?] = argv.map { strdup($0) }
        defer { for pointer in pointers { free(pointer) } }
        pointers.append(nil)

        let status = pointers.withUnsafeMutableBufferPointer { buffer -> Int32 in
            e2_bridge_run(root.path, out.path, Int32(argv.count), buffer.baseAddress)
        }
        let text = (try? String(contentsOf: out, encoding: .utf8)) ?? ""
        return (status, text)
    }

    /// The end-to-end path: interpreter, loader, syscalls and the filesystem, on a device
    /// architecture, inside the app sandbox.
    func testBusyboxRunsInTheApp() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        XCTAssertTrue(FileManager.default.fileExists(atPath: busybox),
                      "Tier D's guest root has no bin/busybox")

        let (status, output) = try runGuest(busybox, ["sh", "-c", "echo tier-d-ok"])
        // The bridge captures stderr as well, so a failure here carries the interpreter's own
        // complaint instead of an empty string.
        XCTAssertEqual(status, 0, "busybox sh exited \(status); output was <<<\(output)>>>")
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "tier-d-ok")
    }

    /// Arithmetic through the shell, which exercises more instructions than echo does.
    func testShellArithmetic() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        let (status, output) = try runGuest(busybox, ["sh", "-c", "echo $((6*7))"])
        XCTAssertEqual(status, 0)
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "42")
    }

    /// Pipes are the part most likely to break in an app, since the harness synthesises them
    /// rather than calling the host's.
    func testPipeBetweenGuestProcesses() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        let (status, output) = try runGuest(busybox, ["sh", "-c", "echo hello | tr a-z A-Z"])
        XCTAssertEqual(status, 0)
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "HELLO")
    }

    /// CPython, in the app.
    ///
    /// This is the point of Tier D: a real interpreter running a real program on a device that
    /// refuses executable pages. It is skipped when the bundle has only the busybox root, which
    /// is the shipping default unless the build fetched Python.
    func testPythonRunsInTheApp() throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.fileExists(atPath: python),
                          "this build's guest root has no CPython")

        let script = root.appendingPathComponent("tmp/e2-pytest.py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        print("python", __import__("sys").version.split()[0])
        print(2 ** 10)
        import json
        print(json.dumps({"k": [1, 2]}))
        """.write(to: script, atomically: true, encoding: .utf8)

        let (status, output) = try runGuest(python, ["/tmp/e2-pytest.py"])
        XCTAssertEqual(status, 0, "python exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("python 3.12"), output)
        XCTAssertTrue(output.contains("1024"), output)
        XCTAssertTrue(output.contains("{\"k\": [1, 2]}"), output)
    }

    /// A file the guest writes has to show up on disk, because the workspace mount depends on
    /// it.
    func testGuestWritesAFile() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        // The guest's /tmp maps under the guest root, so the result is read from there. Aiming
        // at the host's temporary directory tests a path the guest cannot reach.
        let name = "e2-written-\(UUID().uuidString).txt"
        let target = root.appendingPathComponent("tmp").appendingPathComponent(name)
        try? FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: target) }

        let (status, _) = try runGuest(busybox, ["sh", "-c", "echo written > /tmp/\(name)"])
        XCTAssertEqual(status, 0)
        let contents = (try? String(contentsOf: target, encoding: .utf8)) ?? ""
        XCTAssertEqual(contents.trimmingCharacters(in: .whitespacesAndNewlines), "written")
    }
}
