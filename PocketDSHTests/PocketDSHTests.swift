import XCTest
@testable import PocketDSH
import PocketDSHCore
import LuaRuntime

final class PocketDSHTests: XCTestCase {
    func testToolCatalogContainsRequiredLocalTools() {
        let names = Set(ToolCatalog.specs.map(\.function.name))
        XCTAssertTrue(names.contains(ToolNames.listFiles))
        XCTAssertTrue(names.contains(ToolNames.readFile))
        XCTAssertTrue(names.contains(ToolNames.writeFile))
        XCTAssertTrue(names.contains(ToolNames.searchFiles))
        XCTAssertTrue(names.contains(ToolNames.applyPatch))
        XCTAssertTrue(names.contains(ToolNames.runLua))
        XCTAssertTrue(names.contains(ToolNames.runLinux))
        XCTAssertTrue(names.contains(ToolNames.fetchURL))
        XCTAssertTrue(names.contains(ToolNames.todoWrite))
    }

    func testToolSchemasAreObjectsWithRequiredFields() {
        for spec in ToolCatalog.specs {
            guard case .object(let schema) = spec.function.parameters else {
                return XCTFail("Tool schema is not an object: \(spec.function.name)")
            }
            XCTAssertEqual(schema["type"]?.stringValue, "object")
            XCTAssertNotNil(schema["properties"]?.objectValue)
            XCTAssertNotNil(schema["required"])
        }
    }

    func testSystemPromptStatesIPhoneWorkspaceAndNoShell() {
        let prompt = ToolCatalog.systemPrompt(projectName: "Demo")
        XCTAssertTrue(prompt.contains("iPhone"))
        XCTAssertTrue(prompt.contains("real files"))
        XCTAssertTrue(prompt.contains("shell commands"))
    }

    @MainActor
    func testTodoWriteAcceptsOnlyOneInProgressAndRejectsEmptyContent() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let workspace = WorkspaceFileSystem(root: root)
        let executor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore()
        )

        let twoActive = """
        {"todos":[{"content":"a","status":"in_progress"},{"content":"b","status":"in_progress"}]}
        """
        let rejected = await executor.execute(name: ToolNames.todoWrite, argumentsJSON: twoActive)
        XCTAssertTrue(rejected.isError)

        let empty = """{"todos":[{"content":"  ","status":"pending"}]}"""
        XCTAssertTrue(await executor.execute(name: ToolNames.todoWrite, argumentsJSON: empty).isError)

        let good = """{"todos":[{"content":"a","status":"completed"},{"content":"b","status":"in_progress"}]}"""
        let accepted = await executor.execute(name: ToolNames.todoWrite, argumentsJSON: good)
        XCTAssertFalse(accepted.isError)
        XCTAssertEqual(executor.todos.count, 2)
        XCTAssertEqual(executor.todos.first?.status, .completed)
    }

    func testHTMLIsReducedToReadableText() {
        let html = """
        <html><head><style>body{color:red}</style>
        <script>var x = 1 < 2;</script></head>
        <body><h1>Title</h1><p>Some&nbsp;text &amp; more</p></body></html>
        """
        let text = ToolExecutor.plainText(fromHTML: html)
        XCTAssertTrue(text.contains("Title"))
        XCTAssertTrue(text.contains("Some text & more"))
        XCTAssertFalse(text.contains("<h1>"))
        XCTAssertFalse(text.contains("var x"))
        XCTAssertFalse(text.contains("color:red"))
    }

    func testEmbeddedExamplePluginIsValidAndHasRealTools() throws {
        let data = Data(EmbeddedExamplePlugin.manifestJSON.utf8)
        let manifest = try JSONDecoder().decode(PluginManifest.self, from: data)
        try manifest.validate()
        XCTAssertEqual(manifest.permissions, ["workspace.read", "workspace.write"])
        XCTAssertEqual(Set(manifest.tools.map(\.name)), Set(["workspace_report", "write_pocket_note"]))
        XCTAssertTrue(EmbeddedExamplePlugin.luaSource.contains("plugin.workspace_write"))
    }

    @MainActor
    func testCompositePluginToolNameIsModelSafe() {
        let name = PluginRuntimeManager.compositeToolName(
            pluginID: "com.example.workspace-inspector",
            toolName: "workspace_report"
        )
        XCTAssertEqual(name, "plugin__com_example_workspace_inspector__workspace_report")
        XCTAssertTrue(name.range(of: "^[A-Za-z0-9_-]+$", options: .regularExpression) != nil)
    }
}

final class PluginLifecycleTests: XCTestCase {
    private var root: URL!
    private var metadata: URL!
    private var store: PluginStore!
    private var runtime: PluginRuntimeManager!
    private var workspace: WorkspaceFileSystem!

    override func setUpWithError() throws {
        try super.setUpWithError()
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHPluginLifecycle-\(UUID().uuidString)", isDirectory: true)
        root = base.appendingPathComponent("workspace", isDirectory: true)
        metadata = base.appendingPathComponent("metadata", isDirectory: true)
        let pluginRoot = base.appendingPathComponent("plugins", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: metadata, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: pluginRoot, withIntermediateDirectories: true)
        workspace = WorkspaceFileSystem(root: root, metadataRoot: metadata)
        store = PluginStore(root: pluginRoot)
    }

    override func tearDownWithError() throws {
        if let root { try? FileManager.default.removeItem(at: root.deletingLastPathComponent()) }
        try super.tearDownWithError()
    }

    @MainActor
    func testInstallEnableInvokeConfigureDisableUninstall() async throws {
        let installed = try store.installExample()
        XCTAssertTrue(installed.enabled)
        XCTAssertNotNil(store.plugin(id: installed.manifest.id))

        runtime = PluginRuntimeManager(store: store, workspace: workspace)
        let composite = PluginRuntimeManager.compositeToolName(
            pluginID: installed.manifest.id,
            toolName: "write_pocket_note"
        )
        XCTAssertTrue(runtime.toolSpecs.contains { $0.function.name == composite })

        let toolExecutor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            pluginRuntime: runtime,
            systemActions: SystemActionStore()
        )
        let result = await toolExecutor.execute(
            name: composite,
            argumentsJSON: #"{"message":"hello from plugin lifecycle test","path":"PLUGIN_TEST.md"}"#
        )
        XCTAssertFalse(result.isError, result.content)

        let written = try workspace.readText(path: "PLUGIN_TEST.md")
        XCTAssertTrue(written.contains("hello from plugin lifecycle test"))
        XCTAssertTrue(store.logs(for: installed.manifest.id).contains { $0.message.contains("Wrote") })

        store.updateConfig(["prefix": .string("Lifecycle note")], for: installed.manifest.id)
        runtime.reload()
        XCTAssertEqual(store.plugin(id: installed.manifest.id)?.config["prefix"]?.stringValue, "Lifecycle note")

        let configuredResult = await toolExecutor.execute(
            name: composite,
            argumentsJSON: #"{"message":"configured body","path":"CONFIG_TEST.md"}"#
        )
        XCTAssertFalse(configuredResult.isError, configuredResult.content)
        let configuredFile = try workspace.readText(path: "CONFIG_TEST.md")
        XCTAssertTrue(configuredFile.contains("# Lifecycle note"))
        XCTAssertTrue(configuredFile.contains("configured body"))

        store.setEnabled(false, for: installed.manifest.id)
        runtime.reload()
        XCTAssertTrue(runtime.toolSpecs.isEmpty)

        store.uninstall(installed)
        XCTAssertNil(store.plugin(id: installed.manifest.id))
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.directory(for: installed.manifest.id).path))
    }
}

final class SystemActionTests: XCTestCase {
    private var root: URL!
    private var metadata: URL!
    private var workspace: WorkspaceFileSystem!

    override func setUpWithError() throws {
        try super.setUpWithError()
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHSystemActions-\(UUID().uuidString)", isDirectory: true)
        root = base.appendingPathComponent("workspace", isDirectory: true)
        metadata = base.appendingPathComponent("metadata", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: metadata, withIntermediateDirectories: true)
        workspace = WorkspaceFileSystem(root: root, metadataRoot: metadata)
    }

    override func tearDownWithError() throws {
        if let root { try? FileManager.default.removeItem(at: root.deletingLastPathComponent()) }
        try super.tearDownWithError()
    }

    @MainActor
    func testPrepareMessageAndCallRequireUserConfirmation() async throws {
        let actions = SystemActionStore()
        let executor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            pluginRuntime: nil,
            systemActions: actions
        )

        let message = await executor.execute(
            name: ToolNames.prepareMessage,
            argumentsJSON: #"{"recipient":"+15555550123","body":"hello"}"#
        )
        XCTAssertFalse(message.isError, message.content)
        guard case .message(let recipient, let body)? = actions.pending?.kind else {
            return XCTFail("Expected a pending message action")
        }
        XCTAssertEqual(recipient, "+15555550123")
        XCTAssertEqual(body, "hello")

        let invalidAlert = await executor.execute(
            name: ToolNames.scheduleAlert,
            argumentsJSON: #"{"title":"test","due_iso8601":"2000-01-01T00:00:00Z"}"#
        )
        XCTAssertTrue(invalidAlert.isError)

        let call = await executor.execute(
            name: ToolNames.prepareCall,
            argumentsJSON: #"{"number":"+15555550123"}"#
        )
        XCTAssertFalse(call.isError, call.content)
        guard case .call(let number)? = actions.pending?.kind else {
            return XCTFail("Expected a pending call action")
        }
        XCTAssertEqual(number, "+15555550123")
    }
}

final class RuntimeRoutingTests: XCTestCase {
    @MainActor
    func testTierCSelectionNeverPretendsToBeActive() {
        let suite = "PocketDSHRuntimeTests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }

        let runtime = RuntimeCapabilityStore(defaults: defaults)
        // The simulator cannot provide the device JIT entitlement.
        XCTAssertFalse(runtime.jit.isAvailable)
        XCTAssertEqual(runtime.activeTier, .tierA)
        XCTAssertFalse(runtime.wantsTierC)

        runtime.preferredTier = .tierC
        XCTAssertTrue(runtime.wantsTierC)
        // A preference is not an active backend.
        XCTAssertEqual(runtime.activeTier, .tierA)
        XCTAssertTrue(runtime.tierCRequestedButUnavailable)

        // Only a live session flips the reported backend.
        runtime.linuxAvailable = true
        XCTAssertEqual(runtime.activeTier, .tierC)
        XCTAssertFalse(runtime.tierCRequestedButUnavailable)
    }
}
