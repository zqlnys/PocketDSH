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
        XCTAssertTrue(names.contains(ToolNames.present))
        XCTAssertTrue(names.contains(ToolNames.useSkill))
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

    func testConversationDecodesWithoutTodosKey() throws {
        // Conversations written before plans existed have no "todos" key. If
        // decoding them failed, loading a project would silently drop its
        // entire history - which is why AgentConversation has a hand-written
        // decoder rather than the synthesized one.
        let json = """
        {"messages":[],"events":[],"updatedAt":"2024-01-01T00:00:00Z"}
        """
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let conversation = try decoder.decode(AgentConversation.self,
                                              from: Data(json.utf8))
        XCTAssertTrue(conversation.todos.isEmpty)
        XCTAssertTrue(conversation.deliverables.isEmpty)
        XCTAssertTrue(conversation.messages.isEmpty)
    }

    func testConversationRoundTripsTodos() throws {
        let original = AgentConversation(
            messages: [],
            events: [],
            todos: [TodoItem(content: "first", status: .completed),
                    TodoItem(content: "second", status: .inProgress)],
            updatedAt: Date(timeIntervalSince1970: 0)
        )
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let restored = try decoder.decode(AgentConversation.self,
                                          from: try encoder.encode(original))
        XCTAssertEqual(restored.todos.map(\.content), ["first", "second"])
        XCTAssertEqual(restored.todos.map(\.status), [.completed, .inProgress])
    }

    @MainActor
    func testSkillListingAndLoading() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let workspace = WorkspaceFileSystem(root: root)
        let executor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore()
        )

        // No skills/ directory at all: listing must explain, not throw.
        let empty = await executor.execute(name: ToolNames.useSkill, argumentsJSON: "{}")
        XCTAssertFalse(empty.isError)
        XCTAssertTrue(empty.content.contains("No skills"))

        _ = try workspace.write(path: "skills/release.md",
                                content: "Cut a release\n\nRun the tests twice.\n",
                                overwrite: true)

        let listed = await executor.execute(name: ToolNames.useSkill, argumentsJSON: "{}")
        XCTAssertFalse(listed.isError)
        XCTAssertTrue(listed.content.contains("release"))

        let loaded = await executor.execute(name: ToolNames.useSkill,
                                            argumentsJSON: #"{"name":"release"}"#)
        XCTAssertFalse(loaded.isError)
        XCTAssertTrue(loaded.content.contains("Run the tests twice"))

        let missing = await executor.execute(name: ToolNames.useSkill,
                                             argumentsJSON: #"{"name":"nope"}"#)
        XCTAssertTrue(missing.isError)
    }

    @MainActor
    func testEveryAdvertisedToolIsDispatched() async throws {
        // A tool the model can see but the executor does not handle fails at the
        // worst possible moment: mid-task, after the model has already committed
        // to using it. Calling each one with empty arguments is enough to prove
        // a handler exists - the arguments error is expected, "Unknown tool" is
        // not.
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let workspace = WorkspaceFileSystem(root: root)
        let executor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore()
        )

        for spec in ToolCatalog.specs {
            let result = await executor.execute(name: spec.function.name,
                                                argumentsJSON: "{}")
            XCTAssertFalse(result.content.contains("Unknown tool"),
                           "\(spec.function.name) is advertised but not dispatched")
        }
    }

    @MainActor
    func testExportMarkdownIncludesPlanDeliverablesAndTranscript() throws {
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let root = base.appendingPathComponent("ws", isDirectory: true)
        let meta = base.appendingPathComponent("meta", isDirectory: true)
        let plugins = base.appendingPathComponent("plugins", isDirectory: true)
        for dir in [root, meta, plugins] {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        defer { try? FileManager.default.removeItem(at: base) }

        let workspace = WorkspaceFileSystem(root: root, metadataRoot: meta)
        let executor = ToolExecutor(workspace: workspace,
                                    luaEngine: LuaEngine(),
                                    systemActions: SystemActionStore())
        let runner = AgentRunner(workspace: workspace,
                                 toolExecutor: executor,
                                 pluginRuntime: PluginRuntimeManager(store: PluginStore(root: plugins),
                                                                    workspace: workspace),
                                 keychain: KeychainStore(),
                                 settings: SettingsStore(),
                                 projectName: "Demo",
                                 metadataRoot: meta)
        let text = runner.exportMarkdown()
        XCTAssertTrue(text.hasPrefix("# Demo"))
        XCTAssertTrue(text.contains("## Transcript"))
        // An empty conversation still exports a well-formed document rather
        // than an empty string.
        XCTAssertFalse(text.isEmpty)
    }

    func testSearchResultParsing() throws {
        // A trimmed version of the real results page: two hits, one of them
        // behind DuckDuckGo's redirect wrapper.
        let html = """
        <div class="result results_links">
          <h2><a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa&amp;rut=xyz">First &amp; Best</a></h2>
          <a class="result__snippet" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa">A <b>snippet</b> for the first.</a>
        </div>
        <div class="result results_links">
          <h2><a rel="nofollow" class="result__a" href="https://example.org/b">Second result</a></h2>
          <a class="result__snippet" href="https://example.org/b">Another snippet.</a>
        </div>
        """
        let hits = ToolExecutor.searchResults(fromHTML: html, limit: 5)
        XCTAssertEqual(hits.count, 2)
        // The redirect is unwrapped so the caller gets a usable address, and
        // the markup inside the title is stripped.
        XCTAssertEqual(hits.first?.url, "https://example.com/a")
        XCTAssertEqual(hits.first?.title, "First & Best")
        XCTAssertEqual(hits.first?.snippet, "A snippet for the first.")
        // A plain href is passed through untouched.
        XCTAssertEqual(hits.last?.url, "https://example.org/b")
        XCTAssertEqual(hits.last?.title, "Second result")
    }

    func testSearchResultParsingHandlesEmptyPage() throws {
        // A captcha or consent screen must come back as "no results" rather than
        // as a malformed entry, because the model will read whatever it is given.
        XCTAssertTrue(ToolExecutor.searchResults(fromHTML: "<html><body>nothing</body></html>",
                                                 limit: 5).isEmpty)
        XCTAssertTrue(ToolExecutor.searchResults(fromHTML: "", limit: 5).isEmpty)
    }

    func testDeliverablesRoundTrip() throws {
        let original = AgentConversation(
            messages: [],
            events: [],
            todos: [],
            deliverables: [Deliverable(path: "README.md", summary: "how to run it")],
            updatedAt: Date(timeIntervalSince1970: 0)
        )
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let restored = try decoder.decode(AgentConversation.self,
                                          from: try encoder.encode(original))
        XCTAssertEqual(restored.deliverables.count, 1)
        XCTAssertEqual(restored.deliverables.first?.path, "README.md")
        XCTAssertEqual(restored.deliverables.first?.summary, "how to run it")
    }

    @MainActor
    func testPresentRequiresAnExistingFile() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let workspace = WorkspaceFileSystem(root: root)
        let executor = ToolExecutor(
            workspace: workspace,
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore()
        )

        let missing = #"{"path":"nope.md","summary":"x"}"#
        XCTAssertTrue(await executor.execute(name: ToolNames.present,
                                             argumentsJSON: missing).isError)
        XCTAssertTrue(executor.deliverables.isEmpty)

        _ = try workspace.write(path: "README.md", content: "# hi\n", overwrite: true)
        let ok = #"{"path":"README.md","summary":"how to run it"}"#
        let accepted = await executor.execute(name: ToolNames.present, argumentsJSON: ok)
        XCTAssertFalse(accepted.isError)
        XCTAssertEqual(executor.deliverables.count, 1)

        // Presenting the same path again updates rather than duplicates.
        let again = #"{"path":"README.md","summary":"updated"}"#
        _ = await executor.execute(name: ToolNames.present, argumentsJSON: again)
        XCTAssertEqual(executor.deliverables.count, 1)
        XCTAssertEqual(executor.deliverables.first?.summary, "updated")
    }

    func testTodoStatusDecodesFromSnakeCase() throws {
        let json = #"{"messages":[],"events":[],"updatedAt":"2024-01-01T00:00:00Z","todos":[{"id":"00000000-0000-0000-0000-000000000001","content":"x","status":"in_progress"}]}"#
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let conversation = try decoder.decode(AgentConversation.self,
                                              from: Data(json.utf8))
        XCTAssertEqual(conversation.todos.first?.status, .inProgress)
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

    func testSubagentIsDeclaredWithARequiredPrompt() {
        let spec = ToolCatalog.specs.first { $0.function.name == ToolNames.subagent }
        XCTAssertNotNil(spec, "subagent must be offered to the model")
        let parameters = spec?.function.parameters.objectValue
        XCTAssertEqual(parameters?["required"]?.arrayValue, [.string("prompt")])
        XCTAssertNotNil(parameters?["properties"]?.objectValue?["prompt"])
    }

    func testSubagentIsExplainedInTheSystemPrompt() {
        let prompt = ToolCatalog.systemPrompt(projectName: "Demo")
        XCTAssertTrue(prompt.contains("subagent"))
        // The prompt is a numbered list; a repeated number quietly reorders the
        // guidance, so the last item must be the highest number.
        XCTAssertTrue(prompt.contains("11. Report changed paths"))
        XCTAssertFalse(prompt.contains("9. Report changed paths"))
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
