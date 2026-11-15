import Foundation
import LuaRuntime
import PocketDSHCore

@MainActor
final class WorkspaceSession: ObservableObject {
    let project: ProjectRecord
    let rootURL: URL
    let metadataRoot: URL
    let fileSystem: WorkspaceFileSystem
    let pluginRuntime: PluginRuntimeManager
    let agent: AgentRunner

    var id: UUID { project.id }

    private let isSecurityScoped: Bool
    private var didStartAccessing = false

    init(
        project: ProjectRecord,
        location: WorkspaceLocation,
        metadataRoot: URL,
        keychain: KeychainStore,
        settings: SettingsStore,
        pluginStore: PluginStore,
        systemActions: SystemActionStore,
        luaEngine: LuaEngine
    ) throws {
        self.project = project
        self.rootURL = location.root.standardizedFileURL
        self.metadataRoot = metadataRoot.standardizedFileURL
        self.isSecurityScoped = location.isSecurityScoped

        let fileSystem = WorkspaceFileSystem(
            root: rootURL,
            metadataRoot: metadataRoot
        )
        self.fileSystem = fileSystem

        let pluginRuntime = PluginRuntimeManager(
            store: pluginStore,
            workspace: fileSystem
        )
        self.pluginRuntime = pluginRuntime
        let toolExecutor = ToolExecutor(
            workspace: fileSystem,
            luaEngine: luaEngine,
            pluginRuntime: pluginRuntime,
            systemActions: systemActions
        )
        self.agent = AgentRunner(
            workspace: fileSystem,
            toolExecutor: toolExecutor,
            pluginRuntime: pluginRuntime,
            keychain: keychain,
            settings: settings,
            projectName: project.name,
            metadataRoot: metadataRoot
        )

        if isSecurityScoped {
            didStartAccessing = rootURL.startAccessingSecurityScopedResource()
            guard didStartAccessing else {
                throw WorkspaceError.fileSystem(
                    "Could not open the authorized folder. Re-import it from the Files picker."
                )
            }
        }
    }

    deinit {
        if didStartAccessing {
            rootURL.stopAccessingSecurityScopedResource()
        }
    }
}

@MainActor
final class AppState: ObservableObject {
    let projectStore: ProjectStore
    let pluginStore: PluginStore
    let keychain: KeychainStore
    let settings: SettingsStore
    let runtime: RuntimeCapabilityStore
    let systemActions: SystemActionStore
    let luaEngine: LuaEngine

    @Published var activeSession: WorkspaceSession?
    @Published var errorMessage: String?
    @Published var showOnboarding = false
    @Published private(set) var isPluginDemo = false

    init() {
        projectStore = ProjectStore()
        pluginStore = PluginStore()
        keychain = KeychainStore()
        settings = SettingsStore()
        runtime = RuntimeCapabilityStore()
        systemActions = SystemActionStore()
        luaEngine = LuaEngine()
        let arguments = ProcessInfo.processInfo.arguments
        showOnboarding = !keychain.hasKey
        #if DEBUG
        if arguments.contains("-PocketDSHPluginDemo") {
            _ = try? pluginStore.installExample()
            isPluginDemo = true
            showOnboarding = false
        } else if arguments.contains("-PocketDSHSkipOnboarding") {
            showOnboarding = false
        }
        #endif
    }

    func open(_ project: ProjectRecord) {
        do {
            let location = try projectStore.location(for: project)
            let metadata = projectStore.metadataDirectory(for: project.id)
            let session = try WorkspaceSession(
                project: project,
                location: location,
                metadataRoot: metadata,
                keychain: keychain,
                settings: settings,
                pluginStore: pluginStore,
                systemActions: systemActions,
                luaEngine: luaEngine
            )
            projectStore.touch(project)
            activeSession = session
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func closeWorkspace() {
        activeSession = nil
    }
}
