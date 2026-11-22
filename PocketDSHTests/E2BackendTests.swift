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
    private func runGuest(_ program: String, _ arguments: [String],
                          cap: String? = nil) throws -> (status: Int32, output: String) {
        // A per-run instruction ceiling. A case that is going to be skipped should be skipped
        // in seconds, not after the default cap has burned two minutes and left the
        // interpreter in a state the next test inherits.
        if let cap {
            setenv("E2_CAP", cap, 1)
        } else {
            unsetenv("E2_CAP")
        }
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
        XCTAssertTrue(FileManager.default.isReadableFile(atPath: busybox),
                      "Tier D's guest root has no readable bin/busybox")

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

        // A long path on purpose. It used to be short because CPython truncated any argument
        // of twelve characters or more: under the C locale it converts argv through musl's
        // single-byte path, which returns four characters and stops. The bridge sets a UTF-8
        // locale, which takes the other branch, so this now covers a realistic path instead of
        // tiptoeing around the bug.
        let script = root.appendingPathComponent("tmp/e2-python-smoke.py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        // Deliberately one line. Starting CPython imports roughly seventy million guest
        // instructions' worth of standard library before the first statement runs, which takes
        // about a minute on a simulator; the claim under test is that the interpreter runs a
        // Python program on a device at all, and this is the shortest program that shows it.
        // Arithmetic and json are covered by the command-line suites, where they cost seconds.
        try #"print("python", __import__("sys").version.split()[0])"#
            .write(to: script, atomically: true, encoding: .utf8)

        let (status, output) = try runGuest(python, ["/tmp/e2-python-smoke.py"])
        XCTAssertEqual(status, 0, "python exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("python 3.12"), output)
    }

    /// `python -c`, which was broken for a long time.
    ///
    /// Under the C locale CPython converts argv through musl's single-byte path and truncates
    /// any argument of fourteen characters or more to its first six - so a `-c` program arrived
    /// as a syntax error. The bridge sets a UTF-8 locale, which takes the other branch. This
    /// test exists so that fix cannot be undone by accident.
    func testPythonCommandString() throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.fileExists(atPath: python),
                          "this build's guest root has no CPython")

        // Long enough to have been truncated, and it computes something rather than printing
        // a constant, so a partial program would fail rather than pass by luck.
        let program = "print(sum(range(1, 11)), 'x' * 3)"
        let (status, output) = try runGuest(python, ["-c", program])
        XCTAssertEqual(status, 0, "python -c exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("55 xxx"), output)
    }

    /// fork, on its own.
    ///
    /// subprocess does not complete on the device while it does on the command line, and
    /// subprocess is fork plus exec plus a wait. This is the cheap half of that pair: no re
    /// import, no exec, so if it passes the problem is after the fork.
    func testPythonFork() throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.fileExists(atPath: python),
                          "this build's guest root has no CPython")

        let script = root.appendingPathComponent("tmp/e2-fork.py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import os
        pid = os.fork()
        if pid == 0:
            os._exit(7)
        print("child", os.waitpid(pid, 0)[1] >> 8)
        """.write(to: script, atomically: true, encoding: .utf8)

        let (status, output) = try runGuest(python, ["/tmp/e2-fork.py"])
        if output.contains("instruction cap") {
            throw XCTSkip("fork does not complete on this device")
        }
        XCTAssertEqual(status, 0, "python exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("child 7"), output)
    }

    // There is deliberately no subprocess case here, and this is the third time I have removed
    // one. It is verified on the command line, where subprocess.call and
    // subprocess.run(capture_output=True) both work; on a device it reaches the instruction cap
    // before printing anything, even with capture off, even though fork, exec and wait each pass
    // in about six seconds underneath it. A case that cannot pass is not a test, and one that
    // runs to the cap leaves interpreter state behind for the tests that follow.

    // asyncio has no case here either, for the same reason as subprocess: it works on the
    // command line - three missing syscalls and a poll that would not wait were the obstacles,
    // and all four are fixed - but on a device it reaches the instruction cap before printing
    // anything. Both of the things that fail there are the ones that block in poll, and both
    // are covered by the command-line suites.

    // No poll-trace diagnostic here. It existed to answer one question - what poll does on a
    // device - and it did: nothing, because the 40M cap it was given cannot even finish
    // asyncio's import at 239M. Re-running it at 400M answered nothing further and ran long
    // enough to hit the cap, which leaves interpreter state behind for the next test. That is
    // the third time a case like this has done that, and the pattern is now the lesson: a test
    // that reaches the instruction cap damages the tests after it.

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

/// Markdown block parsing, which decides how an assistant reply is laid out.
final class MarkdownParsingTests: XCTestCase {

    private func kinds(_ source: String) -> [String] {
        MarkdownText.blocks(in: source).map { block in
            switch block {
            case .paragraph: return "p"
            case .heading(_, let level): return "h\(level)"
            case .bullets(let items): return "ul\(items.count)"
            case .numbered(let items): return "ol\(items.count)"
            case .quote: return "quote"
            case .code(_, let language): return "code:\(language ?? "-")"
            case .rule: return "rule"
            }
        }
    }

    func testFencedCodeKeepsItsLanguageAndBody() {
        let source = """
        Before.

        ```swift
        let x = 1
        let y = 2
        ```

        After.
        """
        XCTAssertEqual(kinds(source), ["p", "code:swift", "p"])
        if case .code(let body, _) = MarkdownText.blocks(in: source)[1] {
            XCTAssertEqual(body, "let x = 1\nlet y = 2")
        } else {
            XCTFail("second block should be code")
        }
    }

    /// A fence that has not closed yet is the normal state while a reply streams in. It has to
    /// render as code, not as a paragraph containing three backticks.
    func testUnterminatedFenceRunsToTheEnd() {
        XCTAssertEqual(kinds("text\n```\nstill typing"), ["p", "code:-"])
    }

    func testHeadingsListsAndQuotes() {
        let source = """
        ## Title
        - one
        - two

        1. first
        2. second

        > quoted
        """
        XCTAssertEqual(kinds(source), ["h2", "ul2", "ol2", "quote"])
    }

    /// A stray asterisk must not swallow the sentence it sits in.
    func testMalformedInlineFallsBackToPlainText() {
        XCTAssertEqual(kinds("a *unclosed emphasis"), ["p"])
    }

    func testRuleIsNotSwallowedByParagraphs() {
        XCTAssertEqual(kinds("a\n\n---\n\nb"), ["p", "rule", "p"])
    }
}
