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

        var text = "Persistent shell started.\n\n"
        let commands = [
            "pwd",
            "echo persistent-shell-alive",
            "echo tierc-write-ok > /workspace/.tierc-probe.txt",
            "cat /workspace/.tierc-probe.txt",
            "uname -m",
            "ls -la /workspace"
        ]

        for command in commands {
            let result = await Task.detached { session.run(command) }.value
            text += "$ \(command)\n\(result)\n\n"
        }

        text += "still running: \(session.isRunning ? "yes" : "no")\n"
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
