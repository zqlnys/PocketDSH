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
        HStack(spacing: 0) {
            rail
            Divider()
                .overlay(PocketTheme.stroke)
            VStack(spacing: 0) {
                topBar
                Divider()
                    .overlay(PocketTheme.stroke)
                detail
            }
        }
        .background(PocketTheme.background)
        .tint(PocketTheme.accent)
    }

    private var rail: some View {
        VStack(spacing: 6) {
            Button {
                appState.closeWorkspace()
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .frame(width: 44, height: 40)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close project")

            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(PocketTheme.accent)
                    .frame(width: 38, height: 38)
                Text(String(session.project.name.prefix(1)).uppercased())
                    .font(.system(size: 17, weight: .bold, design: .serif))
                    .foregroundStyle(.white)
            }
            .padding(.top, 4)

            VStack(spacing: 4) {
                railButton(.agent)
                railButton(.files)
                railButton(.changes)
                railButton(.plugins)
            }
            .padding(.top, 14)

            Spacer(minLength: 8)

            Circle()
                .fill(runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.amber)
                .frame(width: 6, height: 6)
                .padding(.bottom, 2)
            Text(runtime.activeTier == .tierA ? "A" : "C")
                .font(.caption2.weight(.bold).monospaced())
                .foregroundStyle(PocketTheme.secondaryText)
                .padding(.bottom, 8)

            railButton(.settings)
        }
        .frame(width: 76)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(PocketTheme.canvasElevated)
    }

    private func railButton(_ tab: WorkspaceTab) -> some View {
        Button {
            selection = tab
        } label: {
            VStack(spacing: 3) {
                Image(systemName: tab.icon)
                    .font(.system(size: 16, weight: selection == tab ? .semibold : .regular))
                Text(NSLocalizedString(tab.title, comment: ""))
                    .font(.system(size: 8, weight: .semibold))
            }
            .foregroundStyle(selection == tab ? PocketTheme.accent : PocketTheme.secondaryText)
            .frame(width: 58, height: 48)
            .background {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(selection == tab ? PocketTheme.accent.opacity(0.1) : Color.clear)
            }
            .overlay(alignment: .leading) {
                if selection == tab {
                    Capsule()
                        .fill(PocketTheme.accent)
                        .frame(width: 2, height: 22)
                        .padding(.leading, 2)
                }
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(NSLocalizedString(tab.title, comment: ""))
    }

    private var topBar: some View {
        HStack(spacing: 10) {
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
