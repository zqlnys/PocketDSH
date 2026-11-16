import SwiftUI
import PocketDSHCore

struct AgentView: View {
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var keychain: KeychainStore
    @EnvironmentObject private var settings: SettingsStore
    @EnvironmentObject private var systemActions: SystemActionStore
    @ObservedObject var session: WorkspaceSession
    @ObservedObject var agent: AgentRunner
    let onOpenSettings: () -> Void
    var embedded = false

    @State private var input = ""

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                statusHeader

                if !keychain.hasKey {
                    Banner(
                        icon: "key.fill",
                        title: "DeepSeek API key required",
                        message: "The key is stored in the iOS Keychain, not in the project or conversation.",
                        tint: PocketTheme.amber
                    )
                    .padding(.horizontal, PocketSpacing.lg)
                    .padding(.bottom, PocketSpacing.sm)
                    .onTapGesture(perform: onOpenSettings)
                }

                if let error = agent.errorMessage {
                    Banner(
                        icon: "exclamationmark.triangle.fill",
                        title: "Agent error",
                        message: error,
                        tint: PocketTheme.red
                    )
                    .padding(.horizontal, PocketSpacing.lg)
                    .padding(.bottom, PocketSpacing.sm)
                }

                Divider()
                    .overlay(PocketTheme.stroke.opacity(0.6))

                conversation
                composer
            }
            .background(PocketTheme.background)
            .navigationTitle("Agent")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar(embedded ? .hidden : .visible, for: .navigationBar)
            .sheet(item: $systemActions.pending) { action in
                SystemActionSheet(action: action)
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        appState.closeWorkspace()
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .accessibilityLabel("Close project")
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            onOpenSettings()
                        } label: {
                            Label("Settings", systemImage: "gearshape")
                        }
                        Button(role: .destructive) {
                            agent.resetConversation()
                        } label: {
                            Label("Clear conversation", systemImage: "trash")
                        }
                        .disabled(agent.isRunning || agent.messages.isEmpty)
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("Agent actions")
                }
            }
        }
    }

    private var modelLabel: String {
        switch settings.model {
        case "deepseek-flash": "Flash"
        case "deepseek-v4-pro": "V4 Pro"
        default: settings.model
        }
    }

    private var statusHeader: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(phaseTint.opacity(0.15))
                    .frame(width: 44, height: 44)
                Image(systemName: phaseIcon)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(phaseTint)
            }

            VStack(alignment: .leading, spacing: 3) {
                Text(agent.phase.label)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                Text(embedded ? "Local" : "\(session.project.name) · \(settings.model)")
                    .font(.caption2.monospaced())
                    .foregroundStyle(PocketTheme.secondaryText)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            Menu {
                ForEach(settings.availableModels, id: \.self) { model in
                    Button {
                        settings.model = model
                    } label: {
                        if settings.model == model {
                            Label(model, systemImage: "checkmark")
                        } else {
                            Text(model)
                        }
                    }
                }
            } label: {
                Text(modelLabel)
                    .font(.caption2.monospaced())
                    .foregroundStyle(PocketTheme.secondaryText)
                    .padding(.horizontal, PocketSpacing.sm)
                    .padding(.vertical, 5)
                    .background {
                        RoundedRectangle(cornerRadius: PocketRadius.xs, style: .continuous)
                            .fill(PocketTheme.elevatedSurface)
                    }
            }
            .buttonStyle(.plain)

            Menu {
                ForEach(ReasoningEffort.allCases) { effort in
                    Button {
                        settings.reasoningEffort = effort
                    } label: {
                        if settings.reasoningEffort == effort {
                            Label(effort.title, systemImage: "checkmark")
                        } else {
                            Text(effort.title)
                        }
                    }
                }
            } label: {
                Text(settings.reasoningEffort.title)
                    .font(.caption2.monospaced())
                    .foregroundStyle(PocketTheme.secondaryText)
                    .padding(.horizontal, PocketSpacing.sm)
                    .padding(.vertical, 5)
                    .background {
                        RoundedRectangle(cornerRadius: PocketRadius.xs, style: .continuous)
                            .fill(PocketTheme.elevatedSurface)
                    }
            }
            .buttonStyle(.plain)

            if agent.isRunning {
                Button {
                    agent.cancel()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(PocketTheme.red)
                        .padding(.horizontal, 11)
                        .padding(.vertical, 7)
                        .background(Capsule().fill(PocketTheme.red.opacity(0.12)))
                }
                .buttonStyle(.plain)
            } else {
                StatusPill(text: "Idle", tint: PocketTheme.secondaryText)
            }
        }
        .padding(.horizontal, PocketSpacing.lg)
        .padding(.vertical, 10)
        .background(PocketTheme.surface)
    }

    private var conversation: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 12) {
                    if agent.events.isEmpty {
                        EmptyState(
                            icon: "sparkles",
                            title: "Ask for a real change",
                            message: "Try: “Inspect this project and add a small README section explaining how to run it.” The agent will use file tools on this iPhone."
                        )
                        .padding(.horizontal, PocketSpacing.lg)
                        .padding(.top, 20)
                    } else {
                        ForEach(agent.events) { event in
                            AgentEventRow(
                                event: event,
                                showsDetails: settings.showsToolDetails
                            )
                            .padding(.horizontal, PocketSpacing.lg)
                        }
                    }

                    Color.clear
                        .frame(height: 1)
                        .id("bottom")
                }
                .padding(.vertical, 14)
            }
            .scrollDismissesKeyboard(.interactively)
            .onChange(of: agent.events.count) { _, _ in
                withAnimation(.snappy) {
                    proxy.scrollTo("bottom", anchor: .bottom)
                }
            }
        }
    }

    private var composer: some View {
        VStack(spacing: 8) {
            HStack(alignment: .bottom, spacing: 10) {
                TextField("Describe a task for this workspace…", text: $input, axis: .vertical)
                    .font(.body)
                    .lineLimit(1...5)
                    .padding(.horizontal, 13)
                    .padding(.vertical, 10)
                    .background {
                        RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                            .fill(PocketTheme.elevatedSurface)
                    }
                    .overlay {
                        RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                            .stroke(PocketTheme.stroke.opacity(0.7), lineWidth: 1)
                    }
                    .disabled(agent.isRunning)

                Button {
                    let task = input
                    input = ""
                    agent.send(task)
                } label: {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(Color(hex: "#062018"))
                        .frame(width: 42, height: 42)
                        .background(Circle().fill(PocketTheme.accent))
                        .opacity(canSend ? 1 : 0.35)
                }
                .buttonStyle(PressableButtonStyle())
                .disabled(!canSend)
                .accessibilityLabel("Send task")
            }

            HStack(spacing: 8) {
                Button {
                    settings.thinkingEnabled.toggle()
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: settings.thinkingEnabled ? "brain.head.profile" : "brain")
                        Text("Thinking")
                    }
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(settings.thinkingEnabled ? PocketTheme.accent : PocketTheme.secondaryText)
                }
                .buttonStyle(.plain)

                Text("·")
                    .font(.caption2)
                    .foregroundStyle(PocketTheme.secondaryText)

                Image(systemName: "lock.shield")
                Text("Keychain key · workspace-scoped tools · no shell")
                    .font(.caption2)
                    .foregroundStyle(PocketTheme.secondaryText)
            }
        }
        .padding(.horizontal, PocketSpacing.lg)
        .padding(.top, 9)
        .padding(.bottom, PocketSpacing.sm)
        .background(PocketTheme.surface)
    }

    private var canSend: Bool {
        !agent.isRunning
            && keychain.hasKey
            && !input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var phaseIcon: String {
        switch agent.phase {
        case .idle: return "circle.dotted"
        case .thinking: return "brain.head.profile"
        case .executing: return "hammer.fill"
        case .finished: return "checkmark"
        case .failed: return "exclamationmark"
        case .cancelled: return "stop.fill"
        }
    }

    private var phaseTint: Color {
        switch agent.phase {
        case .idle: return PocketTheme.secondaryText
        case .thinking: return PocketTheme.blue
        case .executing: return PocketTheme.amber
        case .finished: return PocketTheme.accent
        case .failed: return PocketTheme.red
        case .cancelled: return PocketTheme.secondaryText
        }
    }
}

private struct AgentEventRow: View {
    let event: AgentEvent
    let showsDetails: Bool

    /// User turns, tool activity and assistant replies all used to render as the
    /// same card, which left the conversation with no shape: the question, the
    /// answer and a log line about a file read all carried equal weight. They
    /// are now three distinct treatments.
    var body: some View {
        Group {
            if event.kind == .user {
                userBubble
            } else if event.kind == .toolStarted || event.kind == .toolFinished {
                toolRow
            } else {
                messageCard
            }
        }
    }

    private var userText: String {
        event.detail.isEmpty ? event.title : event.detail
    }

    /// Right-aligned and tinted, so a turn reads as something the person said
    /// rather than as another machine event.
    private var userBubble: some View {
        HStack {
            Spacer(minLength: PocketSpacing.xxl)
            VStack(alignment: .trailing, spacing: PocketSpacing.xxs) {
                Text(userText)
                    .font(.subheadline)
                    .foregroundStyle(PocketTheme.primaryText)
                    .multilineTextAlignment(.leading)
                    .textSelection(.enabled)
                Text(event.timestamp.formatted(date: .omitted, time: .shortened))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(PocketTheme.secondaryText.opacity(0.7))
            }
            .padding(.horizontal, PocketSpacing.md)
            .padding(.vertical, PocketSpacing.sm)
            .background {
                RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                    .fill(PocketTheme.accent.opacity(0.10))
            }
        }
    }

    /// Tool traffic is supporting detail, not conversation, so it loses the card
    /// and keeps a single compact line unless the user asked for details.
    private var toolRow: some View {
        VStack(alignment: .leading, spacing: PocketSpacing.xxs) {
            HStack(spacing: PocketSpacing.xs) {
                Image(systemName: icon)
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(tint)
                Text(event.title)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(PocketTheme.secondaryText)
                    .lineLimit(1)
                Spacer(minLength: 0)
                Text(event.timestamp.formatted(date: .omitted, time: .shortened))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(PocketTheme.secondaryText.opacity(0.6))
            }
            if showsDetails && !event.detail.isEmpty {
                Text(event.detail)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(PocketTheme.secondaryText)
                    .lineLimit(6)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(PocketSpacing.sm)
                    .background {
                        RoundedRectangle(cornerRadius: PocketRadius.xs, style: .continuous)
                            .fill(PocketTheme.codeSurface)
                    }
            }
        }
        .padding(.leading, 34)
    }

    private var messageCard: some View {
        HStack(alignment: .top, spacing: PocketSpacing.sm) {
            Image(systemName: icon)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(tint)
                .frame(width: 26, height: 26)
                .background(Circle().fill(tint.opacity(0.12)))

            VStack(alignment: .leading, spacing: PocketSpacing.xs) {
                HStack(spacing: PocketSpacing.xs) {
                    Text(event.title)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(tint)
                    Spacer(minLength: 0)
                    Text(event.timestamp.formatted(date: .omitted, time: .shortened))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(PocketTheme.secondaryText.opacity(0.75))
                }

                if showsDetails && !event.detail.isEmpty {
                    Text(event.detail)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(PocketTheme.primaryText.opacity(0.92))
                        .lineLimit(event.kind == .assistant ? nil : 12)
                        .textSelection(.enabled)
                        .padding(PocketSpacing.sm)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background {
                            RoundedRectangle(cornerRadius: PocketRadius.xs, style: .continuous)
                                .fill(PocketTheme.codeSurface)
                        }
                }
            }
        }
        .padding(PocketSpacing.md)
        .background {
            RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                .fill(PocketTheme.surface)
        }
        .overlay {
            RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                .stroke(PocketTheme.stroke.opacity(0.55), lineWidth: 1)
        }
    }

    private var icon: String {
        switch event.kind {
        case .user: return "person.fill"
        case .assistant: return "sparkles"
        case .toolStarted: return "hammer.fill"
        case .toolFinished: return "terminal.fill"
        case .error: return "exclamationmark.triangle.fill"
        case .cancelled: return "stop.fill"
        case .status: return "circle.dotted"
        }
    }

    private var tint: Color {
        switch event.kind {
        case .user: return PocketTheme.blue
        case .assistant: return PocketTheme.accent
        case .toolStarted: return PocketTheme.amber
        case .toolFinished: return PocketTheme.blue
        case .error: return PocketTheme.red
        case .cancelled: return PocketTheme.secondaryText
        case .status: return PocketTheme.secondaryText
        }
    }
}
