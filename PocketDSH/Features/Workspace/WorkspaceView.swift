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
            Divider()
                .overlay(PocketTheme.stroke)
            detail
        }
        .background(PocketTheme.background)
        .tint(PocketTheme.accent)
    }

    /// Project identity and runtime state, plus the way back out. The tier badge
    /// used to be a bare "A"/"C" beside a 6pt dot in a side rail; it now carries
    /// the words it always meant.
    private var topBar: some View {
        HStack(spacing: PocketSpacing.md) {
            Button {
                appState.closeWorkspace()
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close project")

            ZStack {
                RoundedRectangle(cornerRadius: PocketRadius.sm, style: .continuous)
                    .fill(PocketTheme.accent)
                    .frame(width: 30, height: 30)
                Text(String(session.project.name.prefix(1)))
                    .font(PocketTheme.displayFont(size: 15))
                    .foregroundStyle(.white)
            }

            VStack(alignment: .leading, spacing: 1) {
                Text(session.project.name)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .lineLimit(1)
                Text(NSLocalizedString(runtime.statusLine, comment: ""))
                    .font(.caption2.monospaced())
                    .foregroundStyle(runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.secondaryText)
                    .lineLimit(1)
            }

            Spacer(minLength: PocketSpacing.sm)

            HStack(spacing: PocketSpacing.xs) {
                Circle()
                    .fill(runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.amber)
                    .frame(width: 6, height: 6)
                Text(runtime.activeTier == .tierA ? "Tier A" : "Tier C")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(PocketTheme.secondaryText)
            }
        }
        .padding(.horizontal, PocketSpacing.lg)
        .padding(.vertical, PocketSpacing.sm)
        .background(PocketTheme.surface)
    }

    /// A bottom tab bar rather than the 76pt side rail this used to have. On a
    /// phone the rail consumed a fifth of the width and still had to set its
    /// labels at 8pt to fit, which is well below what is legible; the tab bar
    /// gives that width back to the content and labels at a readable size.
    private var detail: some View {
        TabView(selection: $selection) {
            AgentView(
                session: session,
                agent: session.agent,
                onOpenSettings: { selection = .settings },
                onOpenFile: { _ in selection = .files },
                embedded: true
            )
            .tabItem { Label("Agent", systemImage: "sparkles") }
            .tag(WorkspaceTab.agent)

            FilesView(session: session)
                .tabItem { Label("Files", systemImage: "folder") }
                .tag(WorkspaceTab.files)

            ChangesView(session: session)
                .tabItem { Label("Changes", systemImage: "clock.arrow.circlepath") }
                .tag(WorkspaceTab.changes)

            // PluginsView has no NavigationStack of its own; it used to inherit
            // the one wrapping it in the old layout.
            NavigationStack { PluginsView() }
                .tabItem { Label("Plugins", systemImage: "puzzlepiece.extension") }
                .tag(WorkspaceTab.plugins)

            SettingsView()
                .tabItem { Label("Settings", systemImage: "gearshape") }
                .tag(WorkspaceTab.settings)
        }
    }
}
