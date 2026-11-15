import SwiftUI

enum WorkspaceTab: Hashable {
    case files
    case agent
    case changes
    case plugins
    case settings

    var title: String {
        switch self {
        case .files: "Files"
        case .agent: "Agent"
        case .changes: "Changes"
        case .plugins: "Plugins"
        case .settings: "Settings"
        }
    }

    var icon: String {
        switch self {
        case .files: "folder"
        case .agent: "sparkles"
        case .changes: "clock.arrow.circlepath"
        case .plugins: "puzzlepiece.extension"
        case .settings: "gearshape"
        }
    }
}

struct WorkspaceView: View {
    @ObservedObject var session: WorkspaceSession
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var runtime: RuntimeCapabilityStore
    @State private var selection: WorkspaceTab = .agent

    var body: some View {
        VStack(spacing: 0) {
            topBar
            sectionBar
            Divider()
                .overlay(PocketTheme.stroke)
            detail
        }
        .background(PocketTheme.background)
        .tint(PocketTheme.accent)
    }

    private var topBar: some View {
        HStack(spacing: 12) {
            Button {
                appState.closeWorkspace()
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .frame(width: 36, height: 36)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close project")

            VStack(alignment: .leading, spacing: 2) {
                Text(session.project.name)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .lineLimit(1)
                Text(NSLocalizedString(runtime.statusLine, comment: ""))
                    .font(.caption2.monospaced())
                    .foregroundStyle(runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.secondaryText)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            Button {
                selection = .settings
            } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 16, weight: .regular))
                    .foregroundStyle(PocketTheme.secondaryText)
                    .frame(width: 36, height: 36)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Settings")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(PocketTheme.surface)
    }

    private var sectionBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach([WorkspaceTab.agent, .files, .changes, .plugins, .settings], id: \.self) { tab in
                    Button {
                        selection = tab
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: tab.icon)
                                .font(.system(size: 12, weight: .medium))
                            Text(NSLocalizedString(tab.title, comment: ""))
                                .font(.caption.weight(.semibold))
                        }
                        .foregroundStyle(selection == tab ? PocketTheme.primaryText : PocketTheme.secondaryText)
                        .padding(.horizontal, 11)
                        .padding(.vertical, 8)
                        .background {
                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                .fill(selection == tab ? PocketTheme.elevatedSurface : Color.clear)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
        .background(PocketTheme.surface)
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
}
