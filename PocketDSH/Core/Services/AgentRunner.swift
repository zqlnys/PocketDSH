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

    func cancel() {
        guard isRunning else { return }
        cancelRequested = true
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

    private func runLoop(apiKey: String) async {
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
            for step in 1...maxSteps {
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
                    return
                }

                for call in toolCalls {
                    try Task.checkCancellation()
                    phase = .executing(call.function.name)
                    appendEvent(
                        .toolStarted,
                        title: "Tool · \(call.function.name)",
                        detail: call.function.arguments
                    )

                    let result = await toolExecutor.execute(
                        name: call.function.name,
                        argumentsJSON: call.function.arguments
                    )
                    try Task.checkCancellation()

                    messages.append(
                        LLMMessage(
                            role: .tool,
                            content: result.content,
                            toolCallID: call.id,
                            name: call.function.name
                        )
                    )
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

    private func load() {
        guard let data = try? Data(contentsOf: conversationURL) else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        guard let conversation = try? decoder.decode(AgentConversation.self, from: data) else {
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
