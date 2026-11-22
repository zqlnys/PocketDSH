import SwiftUI

/// Tier D smoke test.
///
/// Tier C has had one of these since it shipped, and the asymmetry mattered: with D selected
/// the settings screen said "not running" and offered no way to find out whether D worked
/// short of asking the agent to run something. This runs the same four things the unit tests
/// run, in the app, on the device.
struct E2SmokeView: View {
    @EnvironmentObject private var appState: AppState
    @State private var output = "Starting the E2 interpreter..."
    @State private var running = true

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Tier D smoke test")
                .font(PocketTheme.displayFont(size: 34))
            Text("E2 · aarch64 · interpreter, not JIT")
                .font(.caption.monospaced())
                .foregroundStyle(PocketTheme.secondaryText)
            ScrollView {
                Text(output)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .padding(PocketSpacing.md)
            .background(PocketTheme.codeSurface)
            .clipShape(RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous))
        }
        .padding(PocketSpacing.xl)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(PocketTheme.background)
        .task {
            await runSmokeTest()
        }
    }

    /// Four checks, each one a layer that could fail on its own:
    ///
    /// 1. The interpreter starts and busybox dispatches an applet from argv[0].
    /// 2. Arithmetic goes through the shell's own evaluator, so more instructions run.
    /// 3. A pipe is synthesised by the harness rather than the host, which is the part most
    ///    likely to behave differently inside a sandbox.
    /// 4. A guest write has to land on disk where the host can read it back. /workspace is the
    ///    one writable directory: everything else resolves inside a read-only bundle.
    private func runSmokeTest() async {
        var lines: [String] = []
        let checks: [(String, String, String)] = [
            ("applet dispatch", "echo tier-d-ok", "tier-d-ok"),
            ("shell arithmetic", "echo $((6*7))", "42"),
            ("pipe", "echo hello | tr a-z A-Z", "HELLO"),
            ("guest write", "echo written > /workspace/e2-smoke.txt && cat /workspace/e2-smoke.txt",
             "written")
        ]

        // Each check has an expected fragment, and a check passes only when the output contains
        // it. Reporting on the exit status alone called a run a success when the guest had just
        // said "can't create /tmp/e2-smoke.txt: Operation not permitted" - the shell carried on
        // to the next command and exited 0.
        var failures = 0
        for (name, command, expected) in checks {
            let result = await appState.e2Runtime.run(command: command)
            let text = result.content.trimmingCharacters(in: .whitespacesAndNewlines)
            if result.isError || !text.contains(expected) {
                failures += 1
                lines.append("\(name): FAILED (wanted \"\(expected)\")\n  \(text)")
            } else {
                lines.append("\(name): \(text)")
            }
        }

        let rootfs = Bundle.main.url(forResource: "alpine-root", withExtension: nil)?.path
            ?? "(not in bundle)"
        lines.append("")
        lines.append("guest root: \(rootfs)")
        lines.append("result: \(failures == 0 ? "all checks passed" : "\(failures) check(s) failed")")
        // Worth stating plainly, because the execution probe on this same screen proves it: no
        // JIT is available on a device, which is why the backend interprets rather than compiles.
        lines.append("")
        lines.append("This ran by interpretation. The execution probe on the main settings screen")
        lines.append("shows why that matters: file-backed and anonymous executable pages are both")
        lines.append("refused on a device, so nothing here compiles code at runtime.")

        output = lines.joined(separator: "\n")
        running = false
    }
}
