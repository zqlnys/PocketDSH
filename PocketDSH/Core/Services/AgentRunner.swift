import Foundation
import PocketDSHCore

enum AgentPhase: Equatable, Sendable {
    case idle
    case thinking(Int)
    case executing(String)
    case finished
    case failed(String)
    case cancelled

    var label: String {
        switch self {
        case .idle:
            return "Ready"
        case .thinking(let step):
            return "Thinking · step \(step)"
        case .executing(let tool):
            return "Running \(tool)"
        case .finished:
            return "Complete"
        case .failed(let message):
            return "Failed: \(message)"
        case .cancelled:
            return "Cancelled"
        }
    }

    var isActive: Bool {
        switch self {
        case .thinking, .executing:
            return true
        default:
            return false
        }
    }
}

@MainActor
final class AgentRunner: ObservableObject {
    @Published private(set) var messages: [LLMMessage] = []
    @Published private(set) var events: [AgentEvent] = []
    /// The plan the model last submitted through todo_write. Shown above the
    /// conversation so a long task shows progress instead of just activity.
    @Published private(set) var todos: [TodoItem] = []
    /// Files the agent marked with `present`, shown as cards above the
    /// conversation so a finished artifact is something to open, not a path in
    /// prose.
    @Published private(set) var deliverables: [Deliverable] = []
    @Published private(set) var phase: AgentPhase = .idle
    @Published private(set) var currentStep = 0
    @Published private(set) var isRunning = false
    @Published var errorMessage: String?

    private let workspace: WorkspaceFileSystem
    private let toolExecutor: ToolExecutor
    private let pluginRuntime: PluginRuntimeManager
    private let keychain: KeychainStore
    private let settings: SettingsStore
    private let projectName: String
    private let conversationURL: URL
    private let client = DeepSeekClient()

    private var runTask: Task<Void, Never>?
    private var cancelRequested = false
    /// How many sub-agents deep this runner is. A sub-agent may delegate further,
    /// but only so far: without a ceiling a model that keeps delegating would
    /// recurse until the process dies, and the failure would look like a hang.
    private var subagentDepth = 0
    static let maxSubagentDepth = 2

    init(
        workspace: WorkspaceFileSystem,
        toolExecutor: ToolExecutor,
        pluginRuntime: PluginRuntimeManager,
        keychain: KeychainStore,
        settings: SettingsStore,
        projectName: String,
        metadataRoot: URL
    ) {
        self.workspace = workspace
        self.toolExecutor = toolExecutor
        self.pluginRuntime = pluginRuntime
        self.keychain = keychain
        self.settings = settings
        self.projectName = projectName
        self.conversationURL = metadataRoot.appendingPathComponent("agent-conversation.json")
        load()
        // The tool owns the validation; the runner owns the state the UI reads.
        toolExecutor.onTodosChanged = { [weak self] items in
            self?.todos = items
            self?.save()
        }
        toolExecutor.onDeliverablesChanged = { [weak self] items in
            self?.deliverables = items
            self?.save()
        }
    }

    deinit {
        runTask?.cancel()
    }

    func send(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, !isRunning else { return }

        guard let key = keychain.apiKey(), !key.isEmpty else {
            errorMessage = "Add a DeepSeek API key in Settings before running the agent."
            return
        }

        errorMessage = nil
        cancelRequested = false
        isRunning = true
        pluginRuntime.reload()
        currentStep = 0
        messages.append(LLMMessage(role: .user, content: trimmed))
        appendEvent(.user, title: "You", detail: trimmed)
        save()

        let apiKey = key
        runTask = Task { [weak self] in
            await self?.runLoop(apiKey: apiKey)
        }
    }

    /// Re-runs the last user turn.
    ///
    /// The assistant's reply and everything after it are dropped first: retrying means
    /// asking the same question again, not appending a second copy of it. The user
    /// message itself stays, so the transcript still reads as a conversation.
    func retryLastMessage() {
        guard !isRunning else { return }
        guard let lastUser = messages.lastIndex(where: { $0.role == .user }) else { return }
        guard let key = keychain.apiKey(), !key.isEmpty else {
            errorMessage = "Add a DeepSeek API key in Settings before running the agent."
            return
        }

        messages = Array(messages.prefix(through: lastUser))
        if let lastUserEvent = events.lastIndex(where: { $0.kind == .user }) {
            events = Array(events.prefix(through: lastUserEvent))
        }
        errorMessage = nil
        cancelRequested = false
        isRunning = true
        pluginRuntime.reload()
        currentStep = 0
        save()

        runTask = Task { [weak self] in
            await self?.runLoop(apiKey: key)
        }
    }

    /// Keeps the transcript inside a budget by eliding the oldest tool results.
    private func trimOldToolResults() {
        messages = AgentRunner.trimmed(messages)
    }

    /// The trimming itself, as a pure function so it can be tested without a runner.
    ///
    /// Only tool results are shortened, and each is replaced rather than removed: a tool message
    /// answers a specific tool call and the API rejects a transcript where the call has no reply.
    /// The newest results stay whole because they are the ones the model is working from.
    static func trimmed(_ messages: [LLMMessage], budget: Int = 240_000,
                        keepRecent: Int = 6) -> [LLMMessage] {
        var total = messages.reduce(0) { $0 + ($1.content ?? "").utf8.count }
        guard total > budget else { return messages }

        var result = messages
        for index in 0..<max(0, result.count - keepRecent) {
            guard total > budget else { break }
            guard result[index].role == .tool else { continue }
            let content = result[index].content ?? ""
            guard content.utf8.count > 400 else { continue }
            let replacement = String(content.prefix(400))
                + "\n\n[trimmed: \(content.utf8.count - 400) bytes of an older result, "
                + "dropped to keep the conversation inside its budget]"
            total -= content.utf8.count - replacement.utf8.count
            result[index] = LLMMessage(
                role: .tool,
                content: replacement,
                toolCallID: result[index].toolCallID,
                name: result[index].name
            )
        }
        return result
    }

    func cancel() {
        guard isRunning else { return }
        cancelRequested = true
        // The shell runs inside a synchronous call that cannot see a cancelled task, so it is told
        // directly. Cancelling the task alone marked the UI cancelled while the interpreter carried
        // on to the instruction ceiling.
        toolExecutor.cancelInFlight()
        runTask?.cancel()
        phase = .cancelled
        appendEvent(.cancelled, title: "Task cancelled", detail: "The user stopped this run.")
        save()
    }

    func resetConversation() {
        guard !isRunning else { return }
        messages = []
        events = []
        phase = .idle
        currentStep = 0
        errorMessage = nil
        save()
    }

    @discardableResult
    private func runLoop(apiKey: String) async -> String? {
        defer {
            isRunning = false
            currentStep = 0
        }

        let system = LLMMessage(
            role: .system,
            content: ToolCatalog.systemPrompt(projectName: projectName)
        )
        let maxSteps = settings.maxSteps

        do {
            // `1...0` traps at runtime, and maxSteps is a settable published
            // property: the initialiser and the stepper both clamp it today,
            // but a closed range over a value the view can write is a crash
            // waiting for the first caller that does not.
            for step in 1...max(1, maxSteps) {
                try Task.checkCancellation()
                currentStep = step
                phase = .thinking(step)

                let requestMessages = [system] + messages
                let assistant = try await client.complete(
                    apiKey: apiKey,
                    model: settings.model,
                    messages: requestMessages,
                    tools: ToolCatalog.specs + pluginRuntime.toolSpecs,
                    thinkingEnabled: settings.thinkingEnabled,
                    reasoningEffort: settings.reasoningEffort
                )
                try Task.checkCancellation()

                guard assistant.content?.isEmpty == false || assistant.toolCalls?.isEmpty == false else {
                    throw DeepSeekClient.DeepSeekError.invalidResponse
                }

                messages.append(assistant)
                if let content = assistant.content, !content.isEmpty {
                    appendEvent(.assistant, title: "Assistant", detail: content)
                }
                if let calls = assistant.toolCalls, !calls.isEmpty {
                    appendEvent(
                        .status,
                        title: "Model requested \(calls.count) tool call(s)",
                        detail: calls.map(\.function.name).joined(separator: ", ")
                    )
                }
                save()

                let toolCalls = assistant.toolCalls ?? []
                guard !toolCalls.isEmpty else {
                    phase = .finished
                    appendEvent(.status, title: "Task complete", detail: "The model returned a final answer.")
                    save()
                    return assistant.content ?? ""
                }

                for call in toolCalls {
                    try Task.checkCancellation()
                    phase = .executing(call.function.name)
                    appendEvent(
                        .toolStarted,
                        title: "Tool · \(call.function.name)",
                        detail: call.function.arguments
                    )

                    let result: ToolExecutionResult
                    if call.function.name == ToolNames.subagent {
                        result = await runSubtaskTool(
                            argumentsJSON: call.function.arguments,
                            apiKey: apiKey
                        )
                    } else {
                        result = await toolExecutor.execute(
                            name: call.function.name,
                            argumentsJSON: call.function.arguments
                        )
                    }
                    try Task.checkCancellation()

                    messages.append(
                        LLMMessage(
                            role: .tool,
                            content: result.content,
                            toolCallID: call.id,
                            name: call.function.name
                        )
                    )
                    // The conversation is what the model is asked to read, and it grew without
                    // limit: every tool result was appended and nothing was ever dropped, so a run
                    // with a few dozen calls carried megabytes - past what any model accepts, and
                    // the request then failed with no hint that the transcript was the reason.
                    trimOldToolResults()
                    appendEvent(
                        .toolFinished,
                        title: "Result · \(call.function.name)",
                        detail: result.content
                    )
                    save()
                }
            }

            phase = .failed("Step limit reached")
            appendEvent(
                .error,
                title: "Step limit reached",
                detail: "The agent used \(maxSteps) steps without a final answer."
            )
            save()
        } catch is CancellationError {
            if !cancelRequested {
                phase = .cancelled
                appendEvent(.cancelled, title: "Task cancelled", detail: "")
                save()
            }
        } catch {
            phase = .failed(error.localizedDescription)
            errorMessage = error.localizedDescription
            appendEvent(.error, title: "Agent error", detail: error.localizedDescription)
            save()
        }
        return nil
    }

    /// Run one `subagent` tool call.
    ///
    /// The child gets its own conversation and its own conversation file, so
    /// nothing it reasons through leaks into the parent's context - only the
    /// answer comes back. It shares the tool executor, so a sub-agent's edits
    /// and plan updates are the same ones the workspace already tracks.
    private func runSubtaskTool(argumentsJSON: String, apiKey: String) async -> ToolExecutionResult {
        guard subagentDepth < Self.maxSubagentDepth else {
            return .failure(
                "subagent nesting limit reached (\(Self.maxSubagentDepth)). Do this work directly instead."
            )
        }
        guard let data = argumentsJSON.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let prompt = (object["prompt"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines),
              !prompt.isEmpty
        else {
            return .failure("subagent requires a non-empty prompt.")
        }
        let label = (object["label"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        appendEvent(.status, title: "Sub-agent started", detail: label ?? String(prompt.prefix(120)))

        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("subagent-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let child = AgentRunner(
            workspace: workspace,
            toolExecutor: toolExecutor,
            pluginRuntime: pluginRuntime,
            keychain: keychain,
            settings: settings,
            projectName: projectName,
            metadataRoot: tempRoot
        )
        child.subagentDepth = subagentDepth + 1
        child.messages.append(LLMMessage(role: .user, content: prompt))
        child.isRunning = true
        let answer = await child.runLoop(apiKey: apiKey)

        appendEvent(
            .status,
            title: "Sub-agent finished",
            detail: label ?? String(prompt.prefix(120))
        )
        guard let answer, !answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return .failure("The sub-agent did not return an answer. Try giving it a narrower task.")
        }
        return .success(answer)
    }

    /// The conversation as Markdown, for sharing or keeping.
    ///
    /// Built from the same events the transcript shows, so what is exported is
    /// what was on screen - including tool calls, which are the part of a run
    /// that is hardest to reconstruct later.
    /// The transcript as Markdown.
    ///
    /// Pure, so the shape of what a user copies or shares can be tested without
    /// standing up a runner: a transcript that silently dropped the plan or the
    /// tool calls would still look plausible in another app.
    nonisolated static func transcriptMarkdown(
        projectName: String,
        todos: [TodoItem],
        deliverables: [Deliverable] = [],
        events: [AgentEvent]
    ) -> String {
        var lines: [String] = ["# \(projectName)", ""]
        if !todos.isEmpty {
            lines.append("## Plan")
            for item in todos {
                let box: String
                switch item.status {
                case .pending: box = "- [ ]"
                case .inProgress: box = "- [~]"
                case .completed: box = "- [x]"
                }
                lines.append("\(box) \(item.content)")
            }
            lines.append("")
        }
        if !deliverables.isEmpty {
            lines.append("## Delivered")
            for item in deliverables {
                lines.append(item.summary.isEmpty
                             ? "- `\(item.path)`"
                             : "- `\(item.path)` — \(item.summary)")
            }
            lines.append("")
        }
        lines.append("## Transcript")
        lines.append("")
        for event in events {
            let stamp = event.timestamp.formatted(date: .omitted, time: .shortened)
            switch event.kind {
            case .user:
                lines.append("**You** (\(stamp))")
                lines.append("")
                lines.append(event.detail.isEmpty ? event.title : event.detail)
                lines.append("")
            case .assistant:
                lines.append("**Assistant** (\(stamp))")
                lines.append("")
                lines.append(event.detail)
                lines.append("")
            case .toolStarted:
                lines.append("- _\(event.title)_ `\(event.detail.prefix(400))`")
            case .toolFinished, .error, .cancelled, .status:
                lines.append("- \(event.title)")
            }
        }
        return lines.joined(separator: "\n")
    }

    /// Instance wrapper so callers keep the short form.
    func exportMarkdown() -> String {
        Self.transcriptMarkdown(
            projectName: projectName,
            todos: todos,
            deliverables: deliverables,
            events: events
        )
    }

    private func appendEvent(_ kind: AgentEvent.Kind, title: String, detail: String) {
        let limit = (kind == .assistant || kind == .user) ? 30_000 : 8_000
        events.append(
            AgentEvent(
                kind: kind,
                title: title,
                detail: String(detail.prefix(limit))
            )
        )
        if events.count > 500 {
            events.removeFirst(events.count - 500)
        }
    }

    private func save() {
        guard !writesBlocked else { return }
        do {
            try FileManager.default.createDirectory(
                at: conversationURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let conversation = AgentConversation(
                messages: messages,
                events: events,
                todos: todos,
                deliverables: deliverables,
                updatedAt: .now
            )
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(conversation)
            try data.write(to: conversationURL, options: .atomic)
        } catch {
            // The in-memory conversation remains usable. A later save can repair
            // persistence without discarding the user's current task.
        }
    }

    /// Set when a saved conversation exists but could not be used. Saving stops, because the
    /// alternative is writing over the file that could not be read.
    private(set) var conversationLoadError: String?
    private var writesBlocked = false

    private func load() {
        // A conversation that cannot be read or decoded is not the same as one that does not
        // exist, and both used to return here silently. The difference matters because the next
        // save writes whatever is in memory: an unreadable file that quietly became an empty
        // conversation would be overwritten by the first thing the user typed.
        guard FileManager.default.fileExists(atPath: conversationURL.path) else { return }
        guard let data = try? Data(contentsOf: conversationURL) else {
            conversationLoadError = "The saved conversation could not be read. It has been left "
                + "on disk; new messages go to a fresh one."
            writesBlocked = true
            return
        }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        guard let conversation = try? decoder.decode(AgentConversation.self, from: data) else {
            // Kept rather than discarded: a decode failure is the one case where the file is
            // worth more than what we would replace it with.
            conversationLoadError = "The saved conversation could not be decoded, so it was not "
                + "loaded and will not be overwritten."
            writesBlocked = true
            return
        }
        messages = conversation.messages
        events = conversation.events
        todos = conversation.todos
        deliverables = conversation.deliverables
        // Keep the executor in step: it validates the next submission against
        // the list it already holds, so a restored plan must live there too.
        toolExecutor.restoreTodos(todos)
        toolExecutor.restoreDeliverables(deliverables)
    }
}
