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

    private func runGuest(_ argv: [String]) throws -> (status: Int32, output: String) {
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

        let (status, output) = try runGuest([busybox, "sh", "-c", "echo tier-d-ok"])
        XCTAssertEqual(status, 0, "busybox sh exited \(status); output was \(output)")
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "tier-d-ok")
    }

    /// Arithmetic through the shell, which exercises more instructions than echo does.
    func testShellArithmetic() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        let (status, output) = try runGuest([busybox, "sh", "-c", "echo $((6*7))"])
        XCTAssertEqual(status, 0)
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "42")
    }

    /// Pipes are the part most likely to break in an app, since the harness synthesises them
    /// rather than calling the host's.
    func testPipeBetweenGuestProcesses() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        let (status, output) = try runGuest([busybox, "sh", "-c", "echo hello | tr a-z A-Z"])
        XCTAssertEqual(status, 0)
        XCTAssertEqual(output.trimmingCharacters(in: .whitespacesAndNewlines), "HELLO")
    }

    /// A file the guest writes has to show up on disk, because the workspace mount depends on
    /// it.
    func testGuestWritesAFile() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        let target = FileManager.default.temporaryDirectory
            .appendingPathComponent("e2-written-\(UUID().uuidString).txt")
        defer { try? FileManager.default.removeItem(at: target) }

        let (status, _) = try runGuest([busybox, "sh", "-c", "echo written > \(target.path)"])
        XCTAssertEqual(status, 0)
        let contents = (try? String(contentsOf: target, encoding: .utf8)) ?? ""
        XCTAssertEqual(contents.trimmingCharacters(in: .whitespacesAndNewlines), "written")
    }
}
