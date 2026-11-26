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

    /// The prompt is the standing instruction that decides which tool the model reaches for, so
    /// what it claims about the backend matters.
    ///
    /// This test used to assert that the prompt contains "shell commands" - as part of the
    /// sentence "shell commands, Node, Python, Xcode, and subprocesses are not in this build". The
    /// backend grew a real shell and a real CPython, the prompt kept denying them, and the test
    /// kept the denial in place. It now asserts the opposite, which is what is true.
    func testSystemPromptDescribesTheBackendThatExists() {
        let prompt = ToolCatalog.systemPrompt(projectName: "Demo")
        XCTAssertTrue(prompt.contains("iPhone"))
        XCTAssertTrue(prompt.contains("real files"))
        XCTAssertTrue(prompt.contains("run_linux"), "the prompt should point at the shell")
        XCTAssertTrue(prompt.contains("CPython 3.12"), "the prompt should say Python is available")
        XCTAssertTrue(prompt.contains("/workspace"), "the prompt should say the project is mounted")
        XCTAssertFalse(prompt.contains("are not in this build"),
                       "the prompt still denies a tool this build has: \(prompt)")
    }

    /// A directory with more entries than a conversation should carry is reported, not dumped.
    ///
    /// The cap and the shell-output cap are the same defect reached by two routes: a tool result
    /// had no upper bound, and nothing fails when one is enormous - it just crowds out everything
    /// else.
    /// A conversation that cannot be written says so.
    ///
    /// Saving happens after every message and its failure was swallowed, so a full disk lost the
    /// whole chat at exit with nothing having reported it. The check below uses the same encoder
    /// path against an unwritable location, which is the closest local stand-in.
    @MainActor
    func testUnwritableConversationPathThrows() throws {
        let conversation = AgentConversation(messages: [], events: [], todos: [],
                                             deliverables: [], updatedAt: .now)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(conversation)
        let blocked = URL(fileURLWithPath: "/dev/null/impossible/conversation.json")
        XCTAssertThrowsError(try data.write(to: blocked, options: .atomic),
                             "writing under /dev/null must fail, or the test proves nothing")
    }

    /// A bad argument string says how it is bad.
    ///
    /// "Must be a JSON object" for a malformed string tells the model nothing it can act on, so it
    /// sends the same thing again and a turn is spent on a message that could have said what was
    /// wrong.
    @MainActor
    func testMalformedArgumentsExplainThemselves() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("args-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let executor = ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            runtimeStore: RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "args")!)
        )

        let broken = await executor.execute(name: ToolNames.listFiles, argumentsJSON: "{")
        XCTAssertTrue(broken.isError)
        XCTAssertFalse(broken.content.contains("must be a JSON object"),
                       "the vague message is back: \(broken.content)")

        let notObject = await executor.execute(name: ToolNames.listFiles, argumentsJSON: "[1, 2]")
        XCTAssertTrue(notObject.isError)
        XCTAssertTrue(notObject.content.contains("array"), notObject.content)

        let empty = await executor.execute(name: ToolNames.listFiles, argumentsJSON: "   ")
        XCTAssertTrue(empty.isError)
        XCTAssertTrue(empty.content.contains("empty"), empty.content)
    }

    /// A tool name that does not exist suggests the one it probably meant.
    ///
    /// "Unknown tool: read_fil" makes the model guess again and spend another turn; naming the near
    /// miss makes it a typo it can correct immediately.
    @MainActor
    func testUnknownToolSuggestsNearMiss() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("unknown-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let executor = ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            runtimeStore: RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "unknown")!)
        )
        let result = await executor.execute(name: "read_fil", argumentsJSON: "{}")
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.content.contains("read_file"),
                      "no suggestion offered: \(result.content)")
    }

    /// The distance function does what the suggestion depends on.
    func testEditDistance() {
        XCTAssertEqual(ToolExecutor.editDistance("read_file", "read_file"), 0)
        XCTAssertEqual(ToolExecutor.editDistance("read_fil", "read_file"), 1)
        XCTAssertEqual(ToolExecutor.editDistance("", "abc"), 3)
    }

    /// A network failure says what to do, not just what happened.
    ///
    /// "The internet connection appears to be offline" leaves a model unable to choose between
    /// retrying, trying another URL, and telling the user - three different decisions.
    func testNetworkFailuresAreActionable() {
        let offline = ToolExecutor.describeNetworkFailure(
            URLError(.notConnectedToInternet))
        XCTAssertTrue(offline.contains("no network"), offline)
        XCTAssertTrue(offline.contains("do not help") || offline.contains("will not help"), offline)

        let timeout = ToolExecutor.describeNetworkFailure(URLError(.timedOut))
        XCTAssertTrue(timeout.contains("timed out"), timeout)

        let host = ToolExecutor.describeNetworkFailure(URLError(.cannotFindHost))
        XCTAssertTrue(host.contains("host"), host)

        // An error that is not a URLError still produces something readable.
        let other = ToolExecutor.describeNetworkFailure(
            NSError(domain: "test", code: 1, userInfo: [NSLocalizedDescriptionKey: "nope"]))
        XCTAssertTrue(other.contains("nope"), other)
    }

    /// A sub-agent's answer is bounded like any other tool result.
    ///
    /// A child that writes a long report would otherwise decide how much of the parent's
    /// conversation it takes, and the parent has no say.
    func testSubagentAnswerIsCapped() {
        let long = String(repeating: "z", count: 50_000)
        let capped = AgentRunner.capped(long)
        XCTAssertLessThan(capped.utf8.count, 34_000)
        XCTAssertTrue(capped.contains("truncated"), capped.suffix(200).description)

        let short = "an answer"
        XCTAssertEqual(AgentRunner.capped(short), short)
    }

    /// An argument the caller asked for and did not get is reported.
    ///
    /// A string where a number belonged used the default as if nothing had been asked for, and an
    /// out-of-range number was clamped without a word - so the model asked for one thing, got
    /// another, and repeated the call.
    @MainActor
    func testClampedArgumentsAreReported() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("clamp-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try "x".write(to: root.appendingPathComponent("a.txt"), atomically: true, encoding: .utf8)

        let executor = ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            runtimeStore: RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "clamp")!)
        )

        let clamped = await executor.execute(name: ToolNames.listFiles,
                                             argumentsJSON: #"{"depth":99}"#)
        XCTAssertFalse(clamped.isError)
        XCTAssertTrue(clamped.content.contains("clamped"), clamped.content)

        let wrongType = await executor.execute(name: ToolNames.listFiles,
                                               argumentsJSON: #"{"depth":"deep"}"#)
        XCTAssertFalse(wrongType.isError)
        XCTAssertTrue(wrongType.content.contains("should be a number"), wrongType.content)

        // The other numeric arguments carry the note too, or the fix only covered one door.
        let readClamped = await executor.execute(name: ToolNames.readFile,
                                                 argumentsJSON: #"{"path":"a.txt","limit":99999}"#)
        XCTAssertFalse(readClamped.isError, readClamped.content)
        XCTAssertTrue(readClamped.content.contains("limit was 99999 and was clamped to 2000"),
                      readClamped.content)

        let luaClamped = await executor.execute(name: ToolNames.runLua,
                                                argumentsJSON: #"{"code":"return 1","timeout_seconds":"soon"}"#)
        XCTAssertFalse(luaClamped.isError, luaClamped.content)
        XCTAssertTrue(luaClamped.content.contains("should be a number"), luaClamped.content)
    }

    /// A plugin argument is an argument, not shell code.
    ///
    /// The old form wrapped in double quotes and escaped only the quote character, so $, backticks
    /// and backslashes reached the shell: an argument containing $(...) was executed. The guest can
    /// only reach the workspace, so the blast radius was small, and that is not the point.
    func testPluginArgumentsAreShellSafe() {
        XCTAssertEqual(E2PythonPluginExecutor.shellQuoted("plain"), "'plain'")
        XCTAssertEqual(E2PythonPluginExecutor.shellQuoted("has space"), "'has space'")
        // The classic injection: a command substitution must stay a literal.
        XCTAssertEqual(E2PythonPluginExecutor.shellQuoted("$(whoami)"), "'$(whoami)'")
        XCTAssertEqual(E2PythonPluginExecutor.shellQuoted("`id`"), "'`id`'")
        // A single quote inside is closed, escaped and reopened.
        XCTAssertEqual(E2PythonPluginExecutor.shellQuoted("it's"), "'it'\\''s'")
    }

    /// The plugin's script goes where the guest looks for it.
    ///
    /// It was written to the process temporary directory while the guest's /workspace was the
    /// runtime's own workspace - two different paths, so the guest was asked to run a file that was
    /// not there.
    @MainActor
    func testPluginWorkspaceMatchesTheGuestView() throws {
        let runtime = E2Runtime()
        let workspace = URL(fileURLWithPath: runtime.resolvedWorkspacePath, isDirectory: true)
        let scratch = workspace.appendingPathComponent("plugin-test.py")

        XCTAssertEqual(scratch.deletingLastPathComponent().path,
                       runtime.resolvedWorkspacePath,
                       "the script must sit in the directory the guest calls /workspace")
        XCTAssertNotEqual(FileManager.default.temporaryDirectory.path,
                          runtime.resolvedWorkspacePath,
                          "these were assumed equal and are not, which was the bug")
    }

    /// The interpreter the guard checks is the one the command names.
    ///
    /// The guard read rootfs/usr/bin/python3.12 while the command said `python3.12` and left it to
    /// the guest's PATH - a check on one thing and a run of another.
    @MainActor
    func testPluginRunsTheInterpreterItChecked() throws {
        let rootfs = E2Runtime().resolvedRootfsPath ?? "/nonexistent"
        let interpreter = (rootfs as NSString).appendingPathComponent("usr/bin/python3.12")
        XCTAssertTrue(interpreter.hasSuffix("/usr/bin/python3.12"),
                      "the checked path and the guest path have to name the same file")
        // And that guest path is what the command uses, not a bare name resolved by PATH.
        XCTAssertTrue("/usr/bin/python3.12".hasPrefix("/usr/bin/"))
    }

    /// A new conversation does not inherit the previous one's plan or its files.
    ///
    /// resetConversation cleared the messages and left the todos, the deliverables and the
    /// executor's own copy of them - so a fresh chat opened with the last run's checklist and a
    /// shelf of files it had never mentioned, and the first `present` counted them.
    @MainActor
    func testResetClearsPlanAndDeliverables() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("reset-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try "x".write(to: root.appendingPathComponent("a.md"), atomically: true, encoding: .utf8)

        let executor = ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            runtimeStore: RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "reset")!)
        )
        let presented = await executor.execute(name: ToolNames.present,
                                               argumentsJSON: #"{"path":"a.md"}"#)
        XCTAssertFalse(presented.isError, presented.content)

        // The plan is stored the same way and has the same problem: todo_write merges by content,
        // so a new conversation's first plan inherited the previous one's statuses.
        _ = await executor.execute(
            name: ToolNames.todoWrite,
            argumentsJSON: #"{"todos":[{"content":"old step","status":"completed"}]}"#)
        XCTAssertFalse(executor.todos.isEmpty)

        executor.resetDeliverables()
        executor.resetTodos()
        XCTAssertTrue(executor.todos.isEmpty, "the plan survived the reset")

        let again = await executor.execute(name: ToolNames.present,
                                           argumentsJSON: #"{"path":"a.md"}"#)
        XCTAssertTrue(again.content.contains("presented 1 file"),
                      "the old list survived the reset: \(again.content)")
    }

    @MainActor
    func testListFilesCapsWhatItReturns() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        for index in 0..<450 {
            try "x".write(to: root.appendingPathComponent("f\(index).txt"),
                          atomically: true, encoding: .utf8)
        }

        let executor = ToolExecutor(
            workspace: WorkspaceFileSystem(root: root),
            luaEngine: LuaEngine(),
            systemActions: SystemActionStore(),
            runtimeStore: RuntimeCapabilityStore(defaults: UserDefaults(suiteName: "cap")!)
        )
        let result = await executor.execute(name: ToolNames.listFiles, argumentsJSON: "{}")
        XCTAssertFalse(result.isError, result.content)
        XCTAssertTrue(result.content.contains("more entries not shown"),
                      "the cap did not engage")
        XCTAssertLessThan(result.content.utf8.count, 200_000,
                          "list_files returned \(result.content.utf8.count) bytes")
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

        let empty = "{\"todos\":[{\"content\":\"  \",\"status\":\"pending\"}]}"
        let emptyRejected = await executor.execute(name: ToolNames.todoWrite, argumentsJSON: empty)
        XCTAssertTrue(emptyRejected.isError)

        let good = "{\"todos\":[{\"content\":\"a\",\"status\":\"completed\"},{\"content\":\"b\",\"status\":\"in_progress\"}]}"
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

        // AgentRunner dispatches subagent itself - it needs the parent conversation and the
        // API key, which the executor does not have - so the executor is the wrong place to
        // look for that one. Everything else has to be handled here.
        //
        // runSubtaskTool is private, so this test cannot prove the exemption is still
        // warranted; if that method is ever removed, this list has to be edited by hand.
        let dispatchedByRunner: Set<String> = [ToolNames.subagent]

        for spec in ToolCatalog.specs {
            if dispatchedByRunner.contains(spec.function.name) { continue }
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

    func testSearchFilesSupportsRegex() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHRegex-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try "alpha 1\nbeta 22\ngamma 333\n".write(
            to: root.appendingPathComponent("data.txt"), atomically: true, encoding: .utf8)

        let workspace = try WorkspaceFileSystem(root: root)
        // Literal search cannot express "a digit run", which is the point of the flag.
        let literal = try workspace.search(query: "[0-9]+", path: ".", useRegex: false)
        XCTAssertTrue(literal.isEmpty, "a literal search must not treat the pattern as a regex")

        let regex = try workspace.search(query: "[0-9]+", path: ".", useRegex: true)
        XCTAssertEqual(regex.count, 3)
        XCTAssertEqual(regex.first?.line, 1)

        // A bad pattern has to be reported, not silently turned into "no matches".
        XCTAssertThrowsError(
            try workspace.search(query: "[unclosed", path: ".", useRegex: true)
        ) { error in
            guard case WorkspaceError.invalidPattern = error else {
                return XCTFail("expected invalidPattern, got \(error)")
            }
        }
    }

    func testSearchFilesStaysLiteralByDefault() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHLiteral-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try "a.b\nabc\n".write(
            to: root.appendingPathComponent("d.txt"), atomically: true, encoding: .utf8)

        let workspace = try WorkspaceFileSystem(root: root)
        // The default must still be a substring match: "a.b" appears once
        // literally but twice as a regex.
        let plain = try workspace.search(query: "a.b", path: ".")
        XCTAssertEqual(plain.count, 1)
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
        let missingRejected = await executor.execute(name: ToolNames.present, argumentsJSON: missing)
        XCTAssertTrue(missingRejected.isError)
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

    func testRetryTruncatesToTheLastUserTurn() {
        // Retrying means asking the same question again, so everything after the last
        // user message has to go - otherwise the model sees its own failed answer and
        // the duplicate question both.
        var messages = [
            LLMMessage(role: .user, content: "first"),
            LLMMessage(role: .assistant, content: "reply one"),
            LLMMessage(role: .user, content: "second"),
            LLMMessage(role: .assistant, content: "bad reply"),
            LLMMessage(role: .user, content: "third"),
            LLMMessage(role: .assistant, content: "another"),
        ]
        let lastUser = messages.lastIndex(where: { $0.role == .user })
        XCTAssertNotNil(lastUser)
        messages = Array(messages.prefix(through: lastUser!))
        XCTAssertEqual(messages.count, 5)
        XCTAssertEqual(messages.last?.content, "third")
        XCTAssertEqual(messages.filter { $0.role == .assistant }.count, 2)
    }

    func testExportedTranscriptContainsTheRun() {
        // Copy and share both hand the user `exportMarkdown()`, so what it contains
        // is the thing worth pinning: a transcript that omits tool calls or the plan
        // would still look plausible but would be useless once pasted elsewhere.
        let markdown = AgentRunner.transcriptMarkdown(
            projectName: "Demo",
            todos: [TodoItem(content: "check the parser", status: .completed)],
            events: [
                AgentEvent(kind: .user, title: "You", detail: "run the tests"),
                AgentEvent(kind: .assistant, title: "Assistant", detail: "on it"),
            ]
        )
        XCTAssertTrue(markdown.contains("# Demo"))
        XCTAssertTrue(markdown.contains("check the parser"))
        XCTAssertTrue(markdown.contains("run the tests"))
        XCTAssertTrue(markdown.contains("on it"))
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
