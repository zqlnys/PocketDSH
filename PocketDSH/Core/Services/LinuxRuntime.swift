import Foundation
import PocketDSHCore

/// Experimental Tier C command runner.
///
/// Commands run through one long-lived emulated `/bin/sh`. The first call pays
/// the emulated kernel startup; every later call reuses the same session, which
/// is what makes a sequence of commands cost ~0.1s each instead of one to three
/// seconds per command.
///
/// A one-shot bridge remains as a fallback: if the session cannot be created the
/// runner still executes the command, just slower.
@MainActor
final class LinuxRuntime: ObservableObject {
    /// What the emulated backend is doing right now. Published so the runtime
    /// status the UI shows is the real state, not a preference.
    enum SessionState: Equatable {
        case idle
        case starting
        case active
        case failed(String)
    }

    @Published private(set) var state: SessionState = .idle

    private var busy = false
    private var rootfsURL: URL?
    private var prepared = false

    /// When true the project is mounted read-only at /workspace. Reads work as
    /// usual; every write returns EROFS. Off by default: the current product
    /// behaviour is read-write, and flipping the default is a product decision.
    var workspaceReadOnly = false

    private var session: PersistentShellSession?
    private var sessionWorkspace: URL?
    private var sessionReadOnly = false
    private var lastSessionError = ""

    /// True when a persistent session is alive and reusable.
    var isSessionActive: Bool {
        session?.isRunning ?? false
    }

    /// Reason the last session start failed, for display in Settings.
    var sessionError: String { lastSessionError }

    /// Runs one command. When `workspace` is provided, the directory is mounted
    /// read-write at `/workspace` inside the guest through iSH's realfs backend,
    /// so the guest sees the real project files rather than a metadata copy.
    func run(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        guard !busy else {
            return .failure("Tier C shell is busy. Wait for the running command to finish.")
        }

        busy = true
        defer { busy = false }

        if let active = await ensureSession(workspace: workspace) {
            let result = await Task.detached { active.run(command, timeout: 30) }.value
            if result.hasPrefix("<shell error") || result.hasPrefix("<shell timeout>") {
                // Either the session died, or the command outran its deadline and
                // left unread output in the pipe. Both leave the session in an
                // unknown state, so drop it; the next call rebuilds cleanly.
                let wasTimeout = result.hasPrefix("<shell timeout>")
                await discardSession()
                state = .failed(wasTimeout ? "command timed out" : "session ended")
                return .failure(
                    wasTimeout
                        ? "Tier C command timed out and the session was reset.\n\(result)"
                        : "Tier C session ended while running the command.\n\(result)"
                )
            }
            return .success(result.isEmpty ? "(no output)" : result)
        }

        // Session unavailable — fall back to the slower one-shot bridge.
        let fallback = await runOneShot(command: command, workspace: workspace)
        if !lastSessionError.isEmpty {
            return fallback.withNote("Persistent session unavailable (\(lastSessionError)); ran once.")
        }
        return fallback
    }

    /// Stops and forgets the session. Called when the workspace changes or when
    /// the user asks Tier C to release its resources.
    func discardSession() async {
        guard let existing = session else { return }
        session = nil
        sessionWorkspace = nil
        sessionReadOnly = false
        state = .idle
        await Task.detached { existing.stop() }.value
    }

    // MARK: - Session lifecycle

    private func ensureSession(workspace: URL?) async -> PersistentShellSession? {
        if let session, session.isRunning,
           sessionWorkspace == workspace,
           sessionReadOnly == workspaceReadOnly {
            return session
        }

        if session != nil {
            await discardSession()
        }

        guard let root = prepareRootfs() else {
            lastSessionError = "rootfs not available in this build"
            state = .failed(lastSessionError)
            return nil
        }

        state = .starting
        var error = ""
        guard let created = PersistentShellSession(
            rootfs: root,
            workspace: workspace,
            workspaceReadOnly: workspaceReadOnly,
            error: &error
        ) else {
            lastSessionError = error.isEmpty ? "shell failed to start" : error
            state = .failed(lastSessionError)
            return nil
        }

        session = created
        sessionWorkspace = workspace
        sessionReadOnly = workspaceReadOnly
        lastSessionError = ""

        // Give the emulated kernel a moment to finish exec'ing /bin/sh before the
        // first command is piped in, so the first write is not dropped.
        try? await Task.sleep(nanoseconds: 300_000_000)
        if !created.isRunning {
            lastSessionError = "shell exited during startup"
            state = .failed(lastSessionError)
            await discardSession()
            return nil
        }
        state = .active
        return created
    }

    // MARK: - One-shot fallback

    private func runOneShot(command: String, workspace: URL?) async -> ToolExecutionResult {
        guard let root = prepareRootfs() else {
            return .failure("Tier C rootfs is not available in this build.")
        }

        let commandToRun = command
        let outputURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("pocketdsh-linux-\(UUID().uuidString).txt")
        let rootPath = root.path
        let outputPath = outputURL.path
        let workspacePath = workspace?.path ?? ""

        Task.detached(priority: .userInitiated) {
            rootPath.withCString { rootPointer in
                commandToRun.withCString { commandPointer in
                    outputPath.withCString { outputPointer in
                        workspacePath.withCString { workspacePointer in
                            _ = pocket_ish_run(rootPointer, workspacePointer, commandPointer, outputPointer)
                        }
                    }
                }
            }
        }

        try? await Task.sleep(nanoseconds: 5_000_000_000)
        let raw = (try? String(contentsOf: outputURL, encoding: .utf8)) ?? ""
        let output = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return .success(
            output.isEmpty
                ? "Linux command started; no output was captured in the observation window."
                : "Linux output (Tier C, one-shot):\n\(output)"
        )
    }

    private func prepareRootfs() -> URL? {
        if prepared, let rootfsURL {
            return rootfsURL
        }
        guard let bundled = Bundle.main.url(forResource: "rootfs", withExtension: nil) else {
            return nil
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PocketDSH", isDirectory: true)
            .appendingPathComponent("Linux", isDirectory: true)
        let root = support.appendingPathComponent("rootfs", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: root.path) {
                try FileManager.default.copyItem(at: bundled, to: root)
            }
        } catch {
            return nil
        }
        prepared = true
        rootfsURL = root
        return root
    }
}

private extension ToolExecutionResult {
    func withNote(_ note: String) -> ToolExecutionResult {
        .success(content + "\n\nNote: \(note)")
    }
}
