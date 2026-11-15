import SwiftUI

enum WorkspaceTab: Hashable {
    case files
    case agent
    case changes
    case settings
}

struct WorkspaceView: View {
    @ObservedObject var session: WorkspaceSession
    @State private var selection: WorkspaceTab = .files

    var body: some View {
        TabView(selection: $selection) {
            FilesView(session: session)
                .tabItem {
                    Label("Files", systemImage: "folder.fill")
                }
                .tag(WorkspaceTab.files)

            AgentView(
                session: session,
                agent: session.agent,
                onOpenSettings: { selection = .settings }
            )
            .tabItem {
                Label("Agent", systemImage: "sparkles")
            }
            .tag(WorkspaceTab.agent)

            ChangesView(session: session)
                .tabItem {
                    Label("Changes", systemImage: "clock.arrow.circlepath")
                }
                .tag(WorkspaceTab.changes)

            SettingsView()
                .tabItem {
                    Label("Settings", systemImage: "gearshape.fill")
                }
                .tag(WorkspaceTab.settings)
        }
        .tint(PocketTheme.accent)
    }
}
