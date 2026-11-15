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
            let workspace = appState.activeSession?.rootURL
            let command = "uname -a; echo; echo '--- /workspace ---'; ls -la /workspace 2>/dev/null; echo; cat /workspace/README.md 2>/dev/null"
            let result = await appState.linuxRuntime.run(
                command: command,
                workspaceRoot: workspace
            )
            output = result.content
        }
    }
}
