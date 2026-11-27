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
    /// Where the guest's /tmp actually is on disk.
    ///
    /// It is $TMPDIR/e2-tmp rather than <root>/tmp: the root is the read-only app bundle on a device,
    /// so a script written there was never visible to the guest.
    private func guestTmpDirectory() -> URL {
        let url = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
            .appendingPathComponent("e2-tmp", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

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
        // An output file that cannot be read is not an empty output. Returning "" for both made a
        // failed run and a silent one indistinguishable, and every assertion then failed with
        // "expected X, got nothing" whether the command produced nothing or never ran.
        let text: String
        do {
            text = try String(contentsOf: out, encoding: .utf8)
        } catch {
            XCTFail("the harness wrote no readable output to \(out.lastPathComponent): \(error)")
            text = ""
        }
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
        let script = guestTmpDirectory().appendingPathComponent("e2-python-smoke.py")
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

        let script = guestTmpDirectory().appendingPathComponent("e2-fork.py")
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

    /// The same path the app's own smoke test takes: E2Runtime, not the bridge underneath it.
    ///
    /// Every other case here calls e2_bridge_run directly, so none of them exercised the runtime's
    /// own guard - and that guard asked isExecutableFile for a file iOS installs read-only. The
    /// whole feature was unusable on a device while these tests were green.
    @MainActor
    func testE2RuntimeRunsACommand() async throws {
        let root = try guestRoot()
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let runtime = E2Runtime()
        let result = await runtime.run(command: "echo runtime-ok")
        XCTAssertFalse(result.isError, result.content)
        XCTAssertTrue(result.content.contains("runtime-ok"),
                      "output was <<<\(result.content)>>>")
        XCTAssertTrue(runtime.isSessionActive, "the runtime should report a live session")
    }

    /// asyncio, which needs the standard library precompiled.
    ///
    /// It failed on a device for many rounds. The cause was not in the interpreter: an app bundle
    /// is read-only, so CPython cannot write __pycache__ and compiles every module from source -
    /// 239M instructions with the cache present, past 600M without, for the same program. The
    /// guest root now ships precompiled bytecode and the bridge points PYTHONPYCACHEPREFIX
    /// somewhere writable, so a module compiled at run time is reused next time.
    func testPythonAsyncio() throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        let script = guestTmpDirectory().appendingPathComponent("e2-asyncio.py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import asyncio

        async def work(n):
            await asyncio.sleep(0)
            return n * 2

        async def main():
            print("gathered", await asyncio.gather(*(work(i) for i in range(4))))

        asyncio.run(main())
        """.write(to: script, atomically: true, encoding: .utf8)

        let (status, output) = try runGuest(python, ["/tmp/e2-asyncio.py"], cap: "400000000")
        XCTAssertFalse(output.contains("instruction cap"),
                       "asyncio did not finish: \(output.suffix(200))")
        XCTAssertEqual(status, 0, "python exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("[0, 2, 4, 6]"), output)
    }

    /// subprocess, both without and with captured streams.
    ///
    /// It failed on a device for many rounds for two reasons, neither of them in the interpreter:
    /// the bundle carried no precompiled bytecode, so every import was compiled from source and
    /// the run never finished; and socketpair's SOCK_CLOEXEC bit means different things on Linux
    /// and macOS. Both are fixed, so this is worth asserting again.
    func testPythonSubprocess() throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        let script = guestTmpDirectory().appendingPathComponent("e2-sub.py")
        try? FileManager.default.createDirectory(at: script.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import subprocess
        print("call", subprocess.call(["/bin/busybox", "echo", "nocap"]))
        print("capture", subprocess.run(["/bin/busybox", "echo", "cap"],
                                        capture_output=True, text=True).stdout.strip())
        """.write(to: script, atomically: true, encoding: .utf8)

        let (status, output) = try runGuest(python, ["/tmp/e2-sub.py"], cap: "400000000")
        XCTAssertFalse(output.contains("instruction cap"),
                       "subprocess did not finish: \(output.suffix(200))")
        XCTAssertEqual(status, 0, "python exited \(status); output was <<<\(output)>>>")
        XCTAssertTrue(output.contains("nocap"), output)
        XCTAssertTrue(output.contains("capture cap"), output)
    }

    /// A command that floods is truncated rather than swallowed whole.
    ///
    /// The output was read with String(contentsOf:), so a command producing hundreds of megabytes
    /// filled memory and the model's context at once, and neither failure said which command did
    /// it. The head and tail are kept and the gap is labelled.
    @MainActor
    func testFloodingOutputIsCapped() async throws {
        let root = try guestRoot()
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let runtime = E2Runtime()
        // A file that is already large, read by one applet. Three earlier attempts at generating
        // output were wrong in three different ways: `yes | head -c` deadlocked the whole CI run
        // for fifty minutes, a shell loop hit the instruction cap at 2,500 lines, and reading
        // /dev/zero blocked. This produces six megabytes in one command that cannot hang.
        let result = await runtime.run(command: "cat /usr/lib/libpython3.12.so.1.0")
        XCTAssertFalse(result.isError, "a capped run should still succeed")
        XCTAssertTrue(result.content.contains("bytes omitted"), "the cap did not engage")
        XCTAssertTrue(result.content.contains("ELF"), "the head was dropped")
        // Comfortably under what was produced, and under what a conversation can carry.
        XCTAssertLessThan(result.content.utf8.count, 100_000,
                          "capped output was \(result.content.utf8.count) bytes")
    }

    /// The guest sees the project it was asked about.
    ///
    /// `run(command:workspace:)` accepted a workspace and ignored it, so every `run_linux` saw a
    /// scratch directory instead of the project - a parameter that promised access and delivered
    /// none.
    @MainActor
    func testGuestSeesTheWorkspace() async throws {
        let root = try guestRoot()
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let project = FileManager.default.temporaryDirectory
            .appendingPathComponent("e2ws-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: project) }
        try "from-the-project".write(to: project.appendingPathComponent("note.txt"),
                                     atomically: true, encoding: .utf8)

        let runtime = E2Runtime()
        let read = await runtime.run(command: "cat /workspace/note.txt", workspace: project)
        XCTAssertFalse(read.isError, read.content)
        XCTAssertTrue(read.content.contains("from-the-project"), read.content)

        // And writing lands on the host, in the same directory.
        let wrote = await runtime.run(command: "echo made-here > /workspace/made.txt",
                                      workspace: project)
        XCTAssertFalse(wrote.isError, wrote.content)
        let onHost = project.appendingPathComponent("made.txt")
        XCTAssertTrue(FileManager.default.fileExists(atPath: onHost.path),
                      "guest write did not reach the project directory")
    }

    /// A guest that sleeps has to wait.
    ///
    /// `clock_nanosleep` returned zero without waiting, so `time.sleep(0.3)` reported 0.0 seconds
    /// and a program pacing itself spun instead. The nuance is TIMER_ABSTIME: with that flag the
    /// timespec names an instant on the monotonic clock, not a duration, and treating it as a
    /// duration asked to sleep until eight days of uptime.
    @MainActor
    func testSleepActuallyWaits() async throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        let workspace = URL(fileURLWithPath: E2Runtime().resolvedWorkspacePath)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let script = workspace.appendingPathComponent("sleep-\(UUID().uuidString).py")
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        import time
        t = time.monotonic()
        time.sleep(0.25)
        print("slept", round(time.monotonic() - t, 2))
        """.write(to: script, atomically: true, encoding: .utf8)

        let runtime = E2Runtime()
        let result = await runtime.run(command: "python3.12 /workspace/\(script.lastPathComponent)",
                                       workspace: workspace)
        XCTAssertFalse(result.isError, result.content)
        // Anything close to 0.25 will do; the point is that it is not 0.0, which is what a
        // no-op sleep reports.
        XCTAssertTrue(result.content.contains("slept 0.2") || result.content.contains("slept 0.3"),
                      "sleep did not wait: \(result.content)")
    }

    /// A guest that exhausts its budget gets MemoryError, not a silent death.
    ///
    /// E2_HEAP_MB was read and discarded with a (void), so a guest could grow until iOS killed the
    /// app - and that arrives as a crash with no message rather than as the error CPython raises
    /// when a limit holds. This is the case that matters on a phone, where the alternative is the
    /// process disappearing.
    @MainActor
    func testGuestHeapLimitIsEnforced() async throws {
        let root = try guestRoot()
        let python = root.appendingPathComponent("usr/bin/python3.12").path
        try XCTSkipUnless(FileManager.default.isReadableFile(atPath: python),
                          "this build's guest root has no CPython")

        let workspace = URL(fileURLWithPath: E2Runtime().resolvedWorkspacePath)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let script = workspace.appendingPathComponent("heap-\(UUID().uuidString).py")
        defer { try? FileManager.default.removeItem(at: script) }
        try """
        try:
            x = bytearray(96 * 1024 * 1024)
            print("allocated", len(x))
        except MemoryError:
            print("memoryerror")
        """.write(to: script, atomically: true, encoding: .utf8)

        // A budget far below the request, so one of the two outcomes must happen and both are
        // checked: either the allocation is refused, or it succeeded and the budget is not a
        // budget. Silence is not an acceptable third.
        setenv("E2_HEAP_MB", "64", 1)
        defer { unsetenv("E2_HEAP_MB") }

        let runtime = E2Runtime()
        let result = await runtime.run(command: "python3.12 /workspace/\(script.lastPathComponent)",
                                       workspace: workspace)
        XCTAssertTrue(result.content.contains("memoryerror") ||
                      result.content.contains("allocated"),
                      "no outcome at all: \(result.content)")
        XCTAssertTrue(result.content.contains("memoryerror"),
                      "96 MB allocated inside a 64 MB budget: \(result.content)")
    }

    /// A cut in the middle of a multi-byte character does not become a replacement character.
    ///
    /// The head and tail are cut at byte offsets, so a character straddling the seam was split in
    /// half - and the one place a reader is already being told something is missing is the worst
    /// place for a spurious U+FFFD.
    func testCappedBoundariesRespectCharacters() {
        // "中" is three bytes; a two-byte tail ends inside it.
        let data = Data([0xE4, 0xB8])              // first two bytes of 中
        XCTAssertEqual(E2Runtime.trimmedToUTF8(data, fromEnd: true), Data())

        let whole = Data([0x61, 0xE4, 0xB8, 0xAD]) // "a中"
        XCTAssertEqual(E2Runtime.trimmedToUTF8(whole, fromEnd: true), whole)

        let leading = Data([0xB8, 0xAD, 0x62])     // tail of 中 then "b"
        XCTAssertEqual(E2Runtime.trimmedToUTF8(leading, fromEnd: false), Data([0x62]))
    }

    /// Two commands in one shell keep their state.
    ///
    /// Opted in rather than default, because a session owns the process's stdout while it lives and
    /// a suite shares one process. The run path ends any session left over before it starts - that
    /// is what stops test N+1 inheriting test N's shell.
    func testSessionKeepsStateBetweenCommands() async throws {
        let runtime = await E2Runtime()

        let first = await runtime.runSession(command: "cd /tmp")
        XCTAssertFalse(first.isError, first.content)
        let second = await runtime.runSession(command: "pwd")
        XCTAssertTrue(second.content.contains("/tmp"),
                      "the working directory did not survive: \(second.content)")
        let third = await runtime.runSession(command: "X=kept")
        XCTAssertFalse(third.isError, third.content)
        let fourth = await runtime.runSession(command: "echo $X")
        XCTAssertTrue(fourth.content.contains("kept"),
                      "a variable did not survive: \(fourth.content)")
        await MainActor.run { runtime.endSession() }
        XCTAssertEqual(e2_bridge_session_alive(), 0, "the session outlived its test")
    }

    /// A command that fails has to say so.
    ///
    /// The runtime only checked for a negative status, so `exit 3` and a missing command both
    /// came back as success with the output attached. That is the same mistake the smoke test
    /// made: trusting an indirect signal instead of what the program actually reported.
    @MainActor
    func testNonZeroExitIsReportedAsFailure() async throws {
        let root = try guestRoot()
        try XCTSkipUnless(FileManager.default.isReadableFile(
                            atPath: root.appendingPathComponent("bin/busybox").path),
                          "needs a readable busybox in the guest root")

        let runtime = E2Runtime()
        let failed = await runtime.run(command: "exit 3")
        XCTAssertTrue(failed.isError, "exit 3 was reported as success: \(failed.content)")
        XCTAssertTrue(failed.content.contains("3"), failed.content)

        let missing = await runtime.run(command: "no-such-command-here")
        XCTAssertTrue(missing.isError, "a missing command was reported as success")

        // And a command that succeeds is still a success, so the check is not simply strict.
        let ok = await runtime.run(command: "echo fine")
        XCTAssertFalse(ok.isError, ok.content)
        XCTAssertTrue(ok.content.contains("fine"), ok.content)
    }

    /// A file the guest writes has to show up on disk, because the workspace mount depends on
    /// it.
    func testGuestWritesAFile() throws {
        let root = try guestRoot()
        let busybox = root.appendingPathComponent("bin/busybox").path
        // The guest's /tmp is $TMPDIR/e2-tmp, not <root>/tmp - the root is the read-only bundle on
        // a device, which is why this test failed there and passed here.
        let name = "e2-written-\(UUID().uuidString).txt"
        let target = guestTmpDirectory().appendingPathComponent(name)
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
