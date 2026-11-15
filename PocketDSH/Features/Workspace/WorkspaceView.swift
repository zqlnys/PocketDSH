import SwiftUI

enum WorkspaceTab: Hashable {
    case files
    case agent
    case changes
    case plugins
    case settings
}

struct WorkspaceView: View {
    @ObservedObject var session: WorkspaceSession
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var runtime: RuntimeCapabilityStore
    @State private var selection: WorkspaceTab = .agent

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                Section("Workspace") {
                    sidebarRow(.agent, title: "Agent", icon: "sparkles")
                    sidebarRow(.files, title: "Files", icon: "folder")
                    sidebarRow(.changes, title: "Changes", icon: "clock.arrow.circlepath")
                }

                Section("Extend") {
                    sidebarRow(.plugins, title: "Plugins", icon: "puzzlepiece.extension")
                }

                Section("System") {
                    sidebarRow(.settings, title: "Settings", icon: "gearshape")
                }
            }
            .listStyle(.sidebar)
            .navigationTitle(session.project.name)
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("RUNTIME")
                        .font(.caption2.weight(.bold).monospaced())
                        .tracking(1.0)
                        .foregroundStyle(PocketTheme.secondaryText)
                    Text(runtime.statusLine)
                        .font(.caption.monospaced())
                        .foregroundStyle(runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.secondaryText)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(16)
                .background(.thinMaterial)
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        appState.closeWorkspace()
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .accessibilityLabel("Close project")
                }
            }
        } detail: {
            detail
        }
        .navigationSplitViewStyle(.balanced)
        .tint(PocketTheme.accent)
    }

    @ViewBuilder
    private var detail: some View {
        switch selection {
        case .files:
            FilesView(session: session)
        case .agent:
            AgentView(
                session: session,
                agent: session.agent,
                onOpenSettings: { selection = .settings }
            )
        case .changes:
            ChangesView(session: session)
        case .plugins:
            NavigationStack {
                PluginsView()
            }
        case .settings:
            SettingsView()
        }
    }

    private func sidebarRow(
        _ tab: WorkspaceTab,
        title: String,
        icon: String
    ) -> some View {
        Label {
            Text(NSLocalizedString(title, comment: ""))
        } icon: {
            Image(systemName: icon)
                .foregroundStyle(selection == tab ? PocketTheme.accent : PocketTheme.secondaryText)
        }
        .tag(tab)
    }
}
