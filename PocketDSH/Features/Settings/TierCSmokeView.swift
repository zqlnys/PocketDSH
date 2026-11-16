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

    /// Verification layers, weakest to strongest:
    ///
    /// 1. Several commands run back to back through the same session the agent
    ///    uses — not a private probe shell.
    /// 2. An external command is placed early: if the child-exit regression comes
    ///    back, the session dies there and later steps report it.
    /// 3. The guest writes into /workspace and the host reads the same path back
    ///    through FileManager; realfs shares inodes, so a matching readback proves
    ///    the mount is a live two-way share.
    private func runSmokeTest() async {
        guard let workspaceURL = appState.activeSession?.rootURL else {
            let result = await appState.linuxRuntime.run(command: "uname -a; cat /etc/alpine-release")
            output = result.content + "\n\n[workspace] no active project session — mount skipped"
            return
        }

        let probeURL = workspaceURL.appendingPathComponent(".tierc-probe.txt")
        try? FileManager.default.removeItem(at: probeURL)

        let readOnly = appState.linuxRuntime.workspaceReadOnly
        var commands = [
            "pwd",
            "uname -m",                                             // external: forks a child
            "echo alive-after-external",                            // session survived it?
            "ls /workspace"                                         // external again
        ]
        if readOnly {
            // Proves the wrapper rejects writes rather than trusting the mount flag.
            commands.append("echo should-fail > /workspace/.tierc-ro-probe.txt; echo write-exit:$?")
        } else {
            commands.append("echo tierc-write-ok > /workspace/.tierc-probe.txt")
            commands.append("read v < /workspace/.tierc-probe.txt; echo got:$v")  // builtin read of the mount
            commands.append("ls /workspace")
        }

        let mode = readOnly ? "read-only" : "read-write"
        var text = "Running \(commands.count) commands through the shared session (\(mode)).\n\n"
        output = text
        let began = Date()

        for (index, command) in commands.enumerated() {
            let result = await appState.linuxRuntime.run(command: command, workspace: workspaceURL)
            let elapsed = String(format: "%.1fs", Date().timeIntervalSince(began))
            text += "[\(index + 1)/\(commands.count) +\(elapsed)] $ \(command)\n\(result.content)\n\n"
            output = text
        }

        text += "session active: \(appState.linuxRuntime.isSessionActive ? "yes" : "no")\n"
        output = text

        let readOnlyProbe = workspaceURL.appendingPathComponent(".tierc-ro-probe.txt")
        if readOnly {
            text += FileManager.default.fileExists(atPath: readOnlyProbe.path)
                ? "\n[read-only check] FAILED — guest wrote into a read-only mount."
                : "\n[read-only check] write rejected, no file created."
        }

        guard !readOnly else {
            try? FileManager.default.removeItem(at: readOnlyProbe)
            output = text
            return
        }

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
