import SwiftUI

struct TierCSmokeView: View {
    @EnvironmentObject private var appState: AppState
    @State private var output = "Starting embedded Alpine Linux..."

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Tier C smoke test")
                .font(.system(size: 34, weight: .medium, design: .serif))
            Text("iSH · Alpine · no JIT · persistent shell")
                .font(.caption.monospaced())
                .foregroundStyle(PocketTheme.secondaryText)
            ScrollView {
                Text(output)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .padding(12)
            .background(PocketTheme.codeSurface)
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(PocketTheme.background)
        .task {
            await runSmokeTest()
        }
    }

    /// Verification has three layers, from weakest to strongest:
    ///
    /// 1. The shell starts and answers several commands in a row — this is what
    ///    the old one-shot bridge could not do (its fixed capture window dropped
    ///    the tail, and it needed a fresh kernel per command).
    /// 2. Command boundaries come from a sentinel the shell echoes itself, so the
    ///    last command's output is captured rather than guessed at.
    /// 3. The guest writes into /workspace and the host reads the same path back
    ///    through FileManager. realfs shares inodes, so a matching readback
    ///    proves the mount is a live two-way share.
    private func runSmokeTest() async {
        guard let workspaceURL = appState.activeSession?.rootURL else {
            let result = await appState.linuxRuntime.run(command: "uname -a; cat /etc/alpine-release")
            output = result.content + "\n\n[workspace] no active project session — mount skipped"
            return
        }

        let probeURL = workspaceURL.appendingPathComponent(".tierc-probe.txt")
        try? FileManager.default.removeItem(at: probeURL)

        let (session, error) = appState.linuxRuntime.makePersistentSession(workspace: workspaceURL)
        guard let session else {
            output = "Persistent shell failed to start: \(error)"
            return
        }

        // Update on every command so a screenshot taken mid-run still shows
        // progress instead of the initial placeholder.
        var text = "Persistent shell started.\n\n"
        output = text
        let commands = [
            "pwd",
            "uname -m",                                    // external: forks a child
            "echo alive-after-external",                   // proves the session survived it
            "echo tierc-write-ok > /workspace/.tierc-probe.txt",
            "read v < /workspace/.tierc-probe.txt; echo got:$v",  // builtin read of the mount
            "ls /workspace"                                // external again
        ]

        let began = Date()
        for (index, command) in commands.enumerated() {
            let result = await Task.detached { session.run(command, timeout: 12) }.value
            let elapsed = String(format: "%.1fs", Date().timeIntervalSince(began))
            text += "[\(index + 1)/\(commands.count) +\(elapsed)] $ \(command)\n\(result)\n\n"
            output = text
        }

        text += "still running: \(session.isRunning ? "yes" : "no")\n"
        output = text
        await Task.detached { session.stop() }.value

        if let written = try? String(contentsOf: probeURL, encoding: .utf8) {
            let value = written.trimmingCharacters(in: .whitespacesAndNewlines)
            text += "\n[host readback] \(probeURL.lastPathComponent) = \(value)"
            text += value == "tierc-write-ok"
                ? "\n[host readback] workspace mount verified: guest write reached the host directory."
                : "\n[host readback] unexpected content."
        } else {
            text += "\n\n[host readback] \(probeURL.lastPathComponent) missing — guest write did not reach the host directory."
        }
        try? FileManager.default.removeItem(at: probeURL)
        output = text
    }
}
