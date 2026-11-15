import SwiftUI

@main
struct PocketDSHApp: App {
    @StateObject private var appState = AppState()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
                .environmentObject(appState.projectStore)
                .environmentObject(appState.pluginStore)
                .environmentObject(appState.runtime)
                .environmentObject(appState.keychain)
                .environmentObject(appState.settings)
                .tint(PocketTheme.accent)
        }
    }
}
