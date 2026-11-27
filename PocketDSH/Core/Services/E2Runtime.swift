import Foundation
// ToolExecutionResult lives in PocketDSHCore, and both branches of the build below return it,
// so the import has to sit outside the conditional.
import PocketDSHCore

#if TIER_D_AVAILABLE

/// Tier D: commands run on E2, the aarch64 interpreter written for this project.
///
/// Unlike Tier C, which emulates x86 through iSH and carries GPLv3, E2 interprets the guest's
/// own instruction set directly and needs no JIT, no fork and no exec - which is why it can
/// live inside an app at all.
///
/// There is no persistent shell here yet: each call starts a fresh guest. For busybox that
/// costs a few milliseconds, so it is not worth faking a session before there is one.
@MainActor
final class E2Runtime: ObservableObject {
    enum SessionState: Equatable {
        case idle
        case starting
        case active
        case failed(String)
    }

    @Published private(set) var state: SessionState = .idle

    /// Set when the rootfs or the interpreter is missing, so Settings can say which.
    private(set) var sessionError = ""

    var isSessionActive: Bool { state == .active }
    var workspaceReadOnly = false

    private var busy = false

    /// The guest root. Bundled into the app; see prepareRootfs.
    private var rootfsURL: URL?

    /// Whether to run each command in a fresh guest or feed one shell.
    ///
    /// Off by default, opted into by the session test.
    ///
    /// Two causes of the earlier hangs are fixed and understood: `e2_rune_main` is not re-entrant, so
    /// a run ends any live session first; and the bridge redirected the process's stdout, so the
    /// guest's streams come from its own descriptor table now.
    ///
    /// What keeps it off is not a hang but semantics: several tests are written against a guest that
    /// starts fresh per command - a pipe between two guest processes, and a flood whose output file
    /// is expected to begin empty. Those pass with a fresh guest and fail with a shell that remembers
    /// the last one. Turning it on means deciding, per caller, which of the two is wanted; that is a
    /// design question, not a switch.
    ///
    /// Earlier note, kept because both causes are still worth knowing: The first: `e2_rune_main` is not
    /// re-entrant, and a session left by one test was still inside it when the next ran - the run
    /// path ends any live session first. The second: the bridge redirected the process's stdout to
    /// the guest's output file, which is process-wide, so the test runner's own output went there
    /// and vanished. The guest's descriptor table points at the files now and nothing else in the
    /// process is touched.
    var usesSession = false

    private var sessionOut: URL?
    private var sessionWorkspacePath: String = ""
    private var sessionMark: URL?
    private var sessionRound = 0

    /// Starts the shell, or returns the URLs of a live one.
    private func startSession(rootfs: URL, busybox: String) -> (out: URL, mark: URL)? {
        if let out = sessionOut, let mark = sessionMark, e2_bridge_session_alive() != 0 {
            return (out, mark)
        }
        let dir = FileManager.default.temporaryDirectory
        let token = UUID().uuidString
        let out = dir.appendingPathComponent("e2-session-\(token).out")
        let mark = dir.appendingPathComponent("e2-session-\(token).mark")
        FileManager.default.createFile(atPath: out.path, contents: nil)
        FileManager.default.createFile(atPath: mark.path, contents: nil)
        let workspace = resolvedWorkspacePath
        sessionWorkspacePath = workspace
        let started = busybox.withCString { bb in
            e2_bridge_session_start(rootfs.path, workspace, out.path, mark.path, bb)
        }
        guard started == 0 else {
            return nil
        }
        sessionOut = out
        sessionMark = mark
        sessionRound = 0
        state = .active
        return (out, mark)
    }

    /// Ends the session, so the next command starts a fresh shell.
    func endSession() {
        e2_bridge_session_close()
        if let out = sessionOut { try? FileManager.default.removeItem(at: out) }
        if let mark = sessionMark { try? FileManager.default.removeItem(at: mark) }
        sessionOut = nil
        sessionMark = nil
        sessionWorkspacePath = ""
        sessionRound = 0
        if case .active = state { state = .idle }
    }

    /// Runs one command in the session's shell and returns what it printed.
    private func runInSession(command: String, out: URL, mark: URL) async -> ToolExecutionResult {
        let before = (try? Data(contentsOf: out))?.count ?? 0
        let round = sessionRound + 1
        let written = command.withCString { e2_bridge_session_write($0) }
        guard written == 0 else {
            endSession()
            return .failure("Tier D lost its shell. The next command will start a new one.")
        }
        // Bounded: a command that never finishes must be reported, not waited on forever.
        let waited = e2_bridge_session_wait(Int32(round), Int32(60_000), mark.path)
        if waited != 0 {
            endSession()
            return waited == -2
                ? .failure("Tier D's shell exited before finishing: \(trimmedCommand(command))")
                : .failure("Tier D's command did not finish within 60 seconds: \(trimmedCommand(command))")
        }
        sessionRound = round

        // Capped like a one-shot run. Reading the whole file meant a command that floods filled the
        // tool result - and the session's file keeps growing across commands, so it is larger than
        // any single one produced.
        guard let handle = try? FileHandle(forReadingFrom: out) else {
            return .failure("Tier D finished but its output could not be read. The command may have "
                + "run; nothing of what it printed survived.")
        }
        defer { try? handle.close() }
        let total = (try? handle.seekToEnd()) ?? 0
        let start = UInt64(before)
        let produced = total > start ? total - start : 0
        if produced == 0 {
            return .success("(\(trimmedCommand(command)) produced no output)")
        }
        // The same head/tail shape readCapped uses: enough to see what happened at both ends, with
        // the middle stated as omitted rather than silently missing.
        let headBytes: UInt64 = 24_000, tailBytes: UInt64 = 8_000
        var text = ""
        if produced <= headBytes + tailBytes {
            try? handle.seek(toOffset: start)
            text = String(decoding: (try? handle.readToEnd()) ?? Data(), as: UTF8.self)
        } else {
            try? handle.seek(toOffset: start)
            let first = (try? handle.read(upToCount: Int(headBytes))) ?? Data()
            try? handle.seek(toOffset: total - tailBytes)
            let last = (try? handle.readToEnd()) ?? Data()
            text = String(decoding: first, as: UTF8.self)
                + "\n… \(produced - headBytes - tailBytes) bytes omitted …\n"
                + String(decoding: last, as: UTF8.self)
        }
        return .success(text)
    }

    private func trimmedCommand(_ command: String) -> String {
        let flat = command.replacingOccurrences(of: "\n", with: " ")
        return flat.count > 60 ? String(flat.prefix(60)) + "…" : flat
    }

    /// Runs a command in a shell kept across calls.
    ///
    /// Separate from `run` rather than a flag on it, because the two answer different questions.
    /// `run` is one command and a guest that starts for it; this is a shell, with what a shell
    /// remembers. The tool wants the second and the tests want the first, and making that explicit
    /// is what kept the suite from having to opt out of a default it never asked for.
    func runSession(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        let previous = usesSession
        usesSession = true
        defer { usesSession = previous }
        return await run(command: command, workspace: workspace)
    }

    func run(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        guard !busy else {
            return .failure("Tier D is busy. Wait for the running command to finish.")
        }
        busy = true
        defer { busy = false }

        guard let rootfs = prepareRootfs() else {
            state = .failed(sessionError)
            return .failure(sessionError)
        }

        let busybox = rootfs.appendingPathComponent("bin/busybox").path
        // Readable, not executable. E2 loads the ELF itself - nothing is exec'd - and Xcode's
        // resource copy does not preserve the executable bit, so requiring it failed on every
        // device with a message saying the file was missing when it was right there.
        guard FileManager.default.isReadableFile(atPath: busybox) else {
            // Say what is actually there. "Reinstall the build" is unhelpful when the file is
            // present but unreadable, and this check has already been wrong once.
            let bin = rootfs.appendingPathComponent("bin")
            let present = (try? FileManager.default.contentsOfDirectory(atPath: bin.path))?
                .sorted().prefix(8).joined(separator: " ") ?? "(no bin directory)"
            let exists = FileManager.default.fileExists(atPath: busybox)
            sessionError = """
            Tier D cannot read \(busybox).
            file exists: \(exists)
            bin contains: \(present)
            """
            state = .failed(sessionError)
            return .failure(sessionError)
        }

        state = .starting
        // One shell across commands, unless the caller asked for a fresh guest. The session keeps
        // the working directory and variables a shell is for; the fresh path stays because a
        // session owns the process's stdout for as long as it lives.
        // A session's /workspace is fixed when it starts, so a different project means a different
        // session. Sharing one made a command look at the previous project's files.
        if usesSession, let want = workspace?.path, want != sessionWorkspacePath {
            endSession()
        }
        if usesSession, let live = startSession(rootfs: rootfs, busybox: busybox) {
            return await runInSession(command: command, out: live.out, mark: live.mark)
        }
        let outURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("e2-\(UUID().uuidString).out")
        defer { try? FileManager.default.removeItem(at: outURL) }

        // The harness follows the usual convention: argv[0] is its own name, argv[1] is the
        // program to load, and the rest belong to the guest. Passing the busybox path at
        // index 0 instead made it try to load a file called "sh" - "cannot load sh".
        let argv: [String] = ["e2", busybox, "sh", "-c", command]
        var cArgs: [UnsafeMutablePointer<CChar>?] = argv.map { strdup($0) }
        defer { for pointer in cArgs { free(pointer) } }
        cArgs.append(nil)

        // The command was asked about a project; without this the guest's /workspace is a scratch
        // directory and the command cannot see a single file of it.
        //
        // A workspace that is not there is reported rather than mounted: the guest would otherwise
        // see an empty directory that does not exist on the host, and every command would fail
        // with ENOENT and no explanation of why.
        if let workspace, !FileManager.default.fileExists(atPath: workspace.path) {
            let message = "The workspace at \(workspace.path) is no longer there. Reopen the "
                + "project, or run the command without one."
            sessionError = message
            state = .failed(message)
            return .failure(message)
        }
        let workspacePath = workspace?.path
        let status = cArgs.withUnsafeMutableBufferPointer { buffer -> Int32 in
            if let workspacePath {
                return workspacePath.withCString { dir in
                    e2_bridge_run_in(rootfs.path, dir, outURL.path,
                                     Int32(argv.count), buffer.baseAddress)
                }
            }
            return e2_bridge_run(rootfs.path, outURL.path, Int32(argv.count), buffer.baseAddress)
        }

        // An output file that cannot be read is not a command with no output. `try?` returned an
        // empty string, so a permissions or storage problem looked exactly like `true`: a success
        // with nothing to show.
        let captured: (text: String, failure: String?)
        captured = readCapped(outURL)
        if let failure = captured.failure, status == 0 {
            sessionError = failure
            state = .failed(failure)
            return .failure(failure)
        }
        let output = captured.text
        if status < 0 {
            sessionError = "Tier D could not start the interpreter."
            state = .failed(sessionError)
            return .failure(sessionError)
        }
        state = .active
        let trimmed = output.trimmingCharacters(in: .newlines)

        // A non-zero status is a failure, and only `status < 0` used to count. `sh -c 'exit 3'`
        // and a command that does not exist both came back as success, which is the same mistake
        // the smoke test made one layer up: trusting an indirect signal instead of the one the
        // program actually produced.
        if status != 0 {
            let body = trimmed.isEmpty ? "(no output)" : trimmed
            let how = status > 128
                ? "was killed by signal \(status - 128)"
                : "exited \(status)"
            return .failure("The command \(how).\n\(body)")
        }
        return .success(trimmed.isEmpty ? "(no output)" : trimmed)
    }

    /// The captured output, capped.
    ///
    /// Reading the whole file worked until a command produced more than a conversation can hold:
    /// `yes` or a verbose build log filled memory and the model's context at once, and neither
    /// failure names itself. The head and the tail are kept, because a log is usually useful at
    /// both ends and least useful in the middle.
    /// Drops a partial UTF-8 sequence at a cut, so the seam does not become a replacement character.
    nonisolated static func trimmedToUTF8(_ data: Data, fromEnd: Bool) -> Data {
        guard !data.isEmpty else { return data }
        let bytes = [UInt8](data)
        if fromEnd {
            // Walk back over continuation bytes to the start of the last character; if that
            // character is incomplete, drop it.
            var index = bytes.count - 1
            var steps = 0
            while index >= 0, bytes[index] & 0xC0 == 0x80, steps < 4 {
                index -= 1
                steps += 1
            }
            guard index >= 0 else { return data }
            let lead = bytes[index]
            let expected: Int
            if lead & 0x80 == 0 { expected = 1 }
            else if lead & 0xE0 == 0xC0 { expected = 2 }
            else if lead & 0xF0 == 0xE0 { expected = 3 }
            else if lead & 0xF8 == 0xF0 { expected = 4 }
            else { return data }
            let have = bytes.count - index
            return have < expected ? Data(bytes[0..<index]) : data
        }
        // At the start, drop leading continuation bytes.
        var index = 0
        while index < bytes.count, index < 3, bytes[index] & 0xC0 == 0x80 { index += 1 }
        return index == 0 ? data : Data(bytes[index...])
    }

    private func readCapped(_ url: URL, head: Int = 24_000,
                            tail: Int = 8_000) -> (text: String, failure: String?) {
        guard let handle = try? FileHandle(forReadingFrom: url) else {
            return ("", "Tier D finished but its output could not be read. The command may have run; "
                + "nothing of what it printed survived.")
        }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        if size <= UInt64(head + tail) {
            try? handle.seek(toOffset: 0)
            let data = (try? handle.readToEnd()) ?? Data()
            return (String(decoding: data, as: UTF8.self), nil)
        }
        try? handle.seek(toOffset: 0)
        let first = (try? handle.read(upToCount: head)) ?? Data()
        try? handle.seek(toOffset: size - UInt64(tail))
        let last = (try? handle.read(upToCount: tail)) ?? Data()
        let omitted = Int(size) - head - tail
        // Split at a character boundary rather than mid-sequence: cutting a multi-byte character
        // in half put a replacement character at the seam, in the one place a reader is already
        // being told that something is missing.
        return (String(decoding: Self.trimmedToUTF8(first, fromEnd: true), as: UTF8.self)
            + "\n… \(omitted) bytes omitted …\n"
            + String(decoding: Self.trimmedToUTF8(last, fromEnd: false), as: UTF8.self), nil)
    }

    /// Stops the run in progress. Called from the cancel path, on any thread.
    ///
    /// The run happens inside one synchronous call, so a cancelled Swift task is invisible to it;
    /// the harness checks this request every 65536 instructions and ends the run with the output it
    /// has so far. Without it, cancelling marked the UI cancelled and the interpreter carried on.
    nonisolated func cancelCurrentRun() {
        e2_bridge_cancel()
    }

    func discardSession() async {
        state = .idle
    }

    /// Finds the guest root inside the app bundle, unpacking it on first use if it ships as
    /// an archive rather than a directory tree.
    /// The guest root, for callers that need to reach into it - the Python plugin executor does,
    /// to name the interpreter.
    var resolvedRootfsPath: String? { prepareRootfs()?.path }

    /// The host directory the guest sees as /workspace.
    ///
    /// It has to be the same one the bridge chooses, and the bridge chooses it from TMPDIR. A
    /// caller that picked its own directory wrote a file the guest could not open, which is a
    /// confusing way to learn that they disagreed.
    var resolvedWorkspacePath: String {
        let tmp = ProcessInfo.processInfo.environment["TMPDIR"]
            ?? NSTemporaryDirectory()
        return (tmp as NSString).appendingPathComponent("e2-workspace")
    }

    private func prepareRootfs() -> URL? {
        if let cached = rootfsURL, FileManager.default.fileExists(atPath: cached.path) {
            return cached
        }
        let bundle = Bundle.main
        // A tree copied into the bundle keeps its name; an archive is expanded to Application
        // Support so the app bundle stays read-only.
        if let tree = bundle.url(forResource: "alpine-root", withExtension: nil) {
            rootfsURL = tree
            return tree
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                               in: .userDomainMask).first?
            .appendingPathComponent("alpine-root", isDirectory: true)
        if let support, FileManager.default.fileExists(atPath: support.path) {
            rootfsURL = support
            return support
        }
        sessionError = "Tier D has no guest root. The build did not include alpine-root."
        return nil
    }
}

#else

@MainActor
final class E2Runtime: ObservableObject {
    enum SessionState: Equatable {
        case idle
        case starting
        case active
        case failed(String)
    }

    @Published private(set) var state: SessionState = .idle
    var isSessionActive: Bool { false }
    var sessionError: String { "This build does not include the E2 backend." }
    var workspaceReadOnly = false

    func run(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        .failure("This build does not include the E2 backend.")
    }

    func discardSession() async {}
}

#endif
