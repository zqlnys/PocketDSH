import SwiftUI

struct RootView: View {
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var settings: SettingsStore

    var body: some View {
        Group {
            if appState.isTierCSmoke {
                TierCSmokeView()
            } else if appState.isPluginDemo {
                NavigationStack {
                    PluginsView()
                }
            } else if let session = appState.activeSession {
                WorkspaceView(session: session)
                    .id(session.id)
            } else {
                ProjectsView()
            }
        }
        .environment(\.locale, settings.language.locale)
        .sheet(isPresented: $appState.showOnboarding) {
            OnboardingView()
        }
        .alert(
            "PocketDSH",
            isPresented: Binding(
                get: { appState.errorMessage != nil },
                set: { if !$0 { appState.errorMessage = nil } }
            )
        ) {
            Button("OK", role: .cancel) {
                appState.errorMessage = nil
            }
        } message: {
            Text(appState.errorMessage ?? "")
        }
    }
}
