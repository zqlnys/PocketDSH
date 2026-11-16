import SwiftUI

/// Answers one question: can this process run code it loaded at runtime?
///
/// The answer picks the shape of a future Linux backend. If aarch64 machine code
/// loaded from a file can execute, a native userland (syscall forwarding only) is
/// viable and the backend is a fraction of the work. If it faults, the backend
/// needs its own instruction interpreter.
///
/// Simulator results do not represent device code-signing policy.
struct ExecProbeView: View {
    @State private var report: ExecutionProbe.Report?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Execution probe")
                .font(PocketTheme.displayFont(size: 34))
            Text("can this process run runtime-loaded code?")
                .font(.caption.monospaced())
                .foregroundStyle(PocketTheme.secondaryText)

            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let report {
                        row("anonymous executable mapping",
                            report.anonymousExecErrno == 0
                                ? "granted"
                                : "denied (errno \(report.anonymousExecErrno))")
                        row("map code file read-only",
                            report.fileMapErrno == 0
                                ? "ok"
                                : "failed (errno \(report.fileMapErrno))")
                        row("mprotect(PROT_EXEC) on file mapping",
                            report.fileProtectErrno == 0
                                ? "granted"
                                : "denied (errno \(report.fileProtectErrno))")
                        row("execute code mapped from file",
                            report.executeStatus == 0
                                ? "returned \(report.executeValue)"
                                : report.executeStatus > 0
                                    ? "faulted (signal \(report.executeStatus))"
                                    : "not attempted")
                        row("map anonymous page writable+executable",
                            report.anonExecMapErrno == 0
                                ? "granted"
                                : "denied (errno \(report.anonExecMapErrno))")
                        row("execute code copied into anonymous page",
                            report.anonExecuteStatus == 0
                                ? "returned \(report.executeValue)"
                                : report.anonExecuteStatus > 0
                                    ? "faulted (signal \(report.anonExecuteStatus))"
                                    : "not attempted (see warning)")

                        Divider().overlay(PocketTheme.stroke)

                        Text(report.verdict)
                            .font(.callout.weight(.medium))
                        Text("Executing code in an anonymous page previously terminated the process on device. That step is opt-in.")
                            .font(.caption)
                            .foregroundStyle(PocketTheme.amber)
                        Text(report.environmentNote)
                            .font(.caption)
                            .foregroundStyle(PocketTheme.amber)
                        Text(report.detail)
                            .font(.caption2.monospaced())
                            .foregroundStyle(PocketTheme.secondaryText)
                            .textSelection(.enabled)
                    } else {
                        Text("Running probe…")
                            .font(.caption.monospaced())
                            .foregroundStyle(PocketTheme.secondaryText)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(PocketSpacing.md)
            .background(PocketTheme.codeSurface)
            .clipShape(RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous))

            Button {
                report = ExecutionProbe.run()
            } label: {
                Label("Run again", systemImage: "arrow.clockwise")
            }
            .buttonStyle(SecondaryButtonStyle())

            // Deliberately behind its own button: on device this step has taken
            // the process down with no catchable signal, which is itself the
            // result we needed.
            Button(role: .destructive) {
                report = ExecutionProbe.run(attemptAnonymousExecution: true)
            } label: {
                Label("Also run anonymous execution (may crash)", systemImage: "exclamationmark.triangle")
            }
            .buttonStyle(SecondaryButtonStyle())
        }
        .padding(PocketSpacing.xl)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(PocketTheme.background)
        .task {
            if report == nil {
                report = ExecutionProbe.run()
            }
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)
            Text(value)
                .font(.caption.monospaced())
        }
    }
}
