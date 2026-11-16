import SwiftUI

struct TierCSmokeView: View {
    @EnvironmentObject private var appState: AppState
    @State private var output = "Starting embedded Alpine Linux..."

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Tier C smoke test")
                .font(.system(size: 34, weight: .medium, design: .serif))
            Text("iSH · Alpine · no JIT")
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

    /// Two independent checks:
    ///
    /// 1. stdout capture — what the guest printed inside the observation window.
    ///    Under i686 emulation a single external command (uname, ls, cat) costs
    ///    one to three seconds, so only the first command or two reliably make
    ///    it into the window. The command is therefore ordered with the
    ///    critical step first.
    /// 2. host readback — the guest writes a file into /workspace, then the host
    ///    reads the same path through FileManager. realfs shares inodes with the
    ///    host, so a successful readback proves the mount is live and writable,
    ///    independent of capture timing. Readback polls instead of reading once,
    ///    because the guest shell keeps running after the window expires.
    private func runSmokeTest() async {
        guard let workspaceURL = appState.activeSession?.rootURL else {
            let result = await appState.linuxRuntime.run(command: "uname -a; cat /etc/alpine-release")
            output = result.content + "\n\n[workspace] no active project session — mount skipped"
            return
        }

        let probeURL = workspaceURL.appendingPathComponent(".tierc-probe.txt")
        try? FileManager.default.removeItem(at: probeURL)

        // `echo` is a shell builtin, so the write completes without waiting on a
        // fork/exec. Put it first so it lands even if later commands overrun.
        let command = "echo tierc-write-ok > /workspace/.tierc-probe.txt; uname -m; ls -la /workspace"
        let result = await appState.linuxRuntime.run(command: command, workspace: workspaceURL)

        var text = result.content
        var capacity = (try? FileManager.default.contentsOfDirectory(atPath: workspaceURL.path)) ?? []
        if !capacity.contains(".tierc-probe.txt") {
            // The guest shell outlives the capture window; give it room to finish.
            for _ in 0..<12 {
                try? await Task.sleep(nanoseconds: 500_000_000)
                capacity = (try? FileManager.default.contentsOfDirectory(atPath: workspaceURL.path)) ?? []
                if capacity.contains(".tierc-probe.txt") { break }
            }
        }

        if let written = try? String(contentsOf: probeURL, encoding: .utf8) {
            let value = written.trimmingCharacters(in: .whitespacesAndNewlines)
            text += "\n\n[host readback] \(probeURL.lastPathComponent) = \(value)"
            text += value == "tierc-write-ok"
                ? "\n[host readback] workspace mount verified: guest write reached the host directory."
                : "\n[host readback] unexpected content."
        } else {
            text += "\n\n[host readback] \(probeURL.lastPathComponent) missing — guest write did not reach the host directory."
            text += "\n[host readback] host directory listing: \(capacity.sorted().joined(separator: ", "))"
        }
        try? FileManager.default.removeItem(at: probeURL)
        output = text
    }
}
