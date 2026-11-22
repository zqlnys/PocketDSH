import SwiftUI
import UIKit
import PocketDSHCore

struct AgentView: View {
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var keychain: KeychainStore
    @EnvironmentObject private var settings: SettingsStore
    @EnvironmentObject private var systemActions: SystemActionStore
    @ObservedObject var session: WorkspaceSession
    @ObservedObject var agent: AgentRunner
    let onOpenSettings: () -> Void
    /// Called when a delivered card is tapped. Optional so the view still works
    /// outside a tab container.
    var onOpenFile: ((String) -> Void)? = nil
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

                deliverableStrip
                planStrip

                Divider()
                    .overlay(PocketTheme.stroke.opacity(0.6))

                conversation
                composer
            }
            .background(PocketTheme.background)
            // A conversation that exists but could not be loaded has to be visible, because the
            // alternative looks exactly like a fresh workspace - and the person would have no way
            // to know their history is still on disk and no longer being written to.
            .safeAreaInset(edge: .top) {
                if let problem = agent.conversationLoadError {
                    Text(problem)
                        .font(.caption)
                        .foregroundStyle(PocketTheme.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(PocketSpacing.md)
                        .background(PocketTheme.field)
                }
            }
            .navigationTitle("Agent")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar(embedded ? .hidden : .visible, for: .navigationBar)
            .sheet(item: $systemActions.pending) { action in
                SystemActionSheet(action: action)
            }
            // A confirmed system action reports what actually happened. Silently closing the
            // sheet leaves the person unable to tell success from a missing permission.
            .overlay(alignment: .top) {
                if let outcome = systemActions.lastOutcome {
                    Text(outcome)
                        .font(.caption)
                        .foregroundStyle(PocketTheme.primaryText)
                        .padding(.horizontal, PocketSpacing.md)
                        .padding(.vertical, PocketSpacing.sm)
                        .background {
                            Capsule().fill(PocketTheme.surface)
                        }
                        .overlay {
                            Capsule().strokeBorder(PocketTheme.stroke.opacity(0.6), lineWidth: 1)
                        }
                        .padding(.horizontal, PocketSpacing.lg)
                        .padding(.top, PocketSpacing.sm)
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .onTapGesture { systemActions.lastOutcome = nil }
                        .task {
                            try? await Task.sleep(nanoseconds: 5_000_000_000)
                            withAnimation { systemActions.lastOutcome = nil }
                        }
                }
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
                        ShareLink(item: agent.exportMarkdown()) {
                            Label("Export transcript", systemImage: "square.and.arrow.up")
                        }
                        .disabled(agent.events.isEmpty)

                        // Copying is a different errand from sharing: it stays on the
                        // device, and the pasteboard round-trips faster than the share
                        // sheet when the destination is another app on this phone.
                        Button {
                            UIPasteboard.general.string = agent.exportMarkdown()
                        } label: {
                            Label("Copy transcript", systemImage: "doc.on.doc")
                        }
                        .disabled(agent.events.isEmpty)

                        // Re-asking the same question is the common case after a bad
                        // answer, and doing it by hand means retyping or copying it.
                        Button {
                            agent.retryLastMessage()
                        } label: {
                            Label("Retry last message", systemImage: "arrow.clockwise")
                        }
                        .disabled(agent.isRunning || !agent.messages.contains { $0.role == .user })

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

    /// Finished artifacts, above the plan. A card is something to open, so each
    /// one is a button rather than a label: tapping it moves to the file.
    @ViewBuilder
    private var deliverableStrip: some View {
        if !agent.deliverables.isEmpty {
            VStack(alignment: .leading, spacing: PocketSpacing.xs) {
                Text("Delivered")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(PocketTheme.secondaryText)

                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: PocketSpacing.sm) {
                        ForEach(agent.deliverables) { item in
                            Button {
                                onOpenFile?(item.path)
                            } label: {
                                VStack(alignment: .leading, spacing: PocketSpacing.xxs) {
                                    HStack(spacing: PocketSpacing.xxs) {
                                        Image(systemName: "doc.text")
                                            .font(.system(size: 10, weight: .semibold))
                                            .foregroundStyle(PocketTheme.accent)
                                        Text(Self.fileName(item.path))
                                            .font(.caption.weight(.semibold))
                                            .foregroundStyle(PocketTheme.primaryText)
                                            .lineLimit(1)
                                    }
                                    if !item.summary.isEmpty {
                                        Text(item.summary)
                                            .font(.caption2)
                                            .foregroundStyle(PocketTheme.secondaryText)
                                            .lineLimit(2)
                                            .multilineTextAlignment(.leading)
                                    }
                                }
                                .frame(width: 168, alignment: .leading)
                                .padding(PocketSpacing.sm)
                                .background {
                                    RoundedRectangle(cornerRadius: PocketRadius.md,
                                                     style: .continuous)
                                        .fill(PocketTheme.surface)
                                }
                                .overlay {
                                    RoundedRectangle(cornerRadius: PocketRadius.md,
                                                     style: .continuous)
                                        .stroke(PocketTheme.cardStroke, lineWidth: 1)
                                }
                            }
                            .buttonStyle(PressableButtonStyle())
                            .accessibilityLabel("Open \(item.path)")
                        }
                    }
                }
            }
            .padding(.horizontal, PocketSpacing.lg)
            .padding(.bottom, PocketSpacing.sm)
        }
    }

    private static func fileName(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }

    private var planDoneCount: Int {
        agent.todos.filter { $0.status == .completed }.count
    }

    /// The agent's plan, when it has one. A long task otherwise shows nothing
    /// but an ever-growing transcript, so this is the one place the shape of the
    /// work is visible without reading back through it.
    @ViewBuilder
    private var planStrip: some View {
        if !agent.todos.isEmpty {
            VStack(alignment: .leading, spacing: PocketSpacing.xs) {
                HStack(spacing: PocketSpacing.xs) {
                    Text("Plan")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(PocketTheme.secondaryText)
                    Spacer(minLength: 0)
                    Text("\(planDoneCount)/\(agent.todos.count)")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(PocketTheme.secondaryText)
                }

                ProgressView(value: Double(planDoneCount),
                             total: Double(max(agent.todos.count, 1)))
                    .tint(PocketTheme.accent)

                ForEach(Array(agent.todos.prefix(4))) { item in
                    HStack(alignment: .firstTextBaseline, spacing: PocketSpacing.xs) {
                        Image(systemName: Self.planIcon(for: item.status))
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundStyle(Self.planTint(for: item.status))
                        Text(item.content)
                            .font(.caption)
                            .foregroundStyle(item.status == .completed
                                             ? PocketTheme.secondaryText
                                             : PocketTheme.primaryText)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                }

                if agent.todos.count > 4 {
                    Text("+\(agent.todos.count - 4)")
                        .font(.caption2)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
            .padding(PocketSpacing.md)
            .background {
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .fill(PocketTheme.surface)
            }
            .overlay {
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .stroke(PocketTheme.stroke.opacity(0.7), lineWidth: 1)
            }
            .padding(.horizontal, PocketSpacing.lg)
            .padding(.bottom, PocketSpacing.sm)
        }
    }

    private static func planIcon(for status: TodoStatus) -> String {
        switch status {
        case .pending: return "circle"
        case .inProgress: return "circle.dotted"
        case .completed: return "checkmark.circle.fill"
        }
    }

    private static func planTint(for status: TodoStatus) -> Color {
        switch status {
        case .pending: return PocketTheme.secondaryText
        case .inProgress: return PocketTheme.accent
        case .completed: return PocketTheme.accent
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
                    // A caption-sized chip is a small target; this brings the
                    // tappable height near 44pt without making it look bulky.
                    .padding(.vertical, 11)
                    .contentShape(Rectangle())
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
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background {
                        // A soft filled pill rather than an outlined box: the composer is the
                        // one control always on screen, and an outline makes it look like a
                        // form field.
                        RoundedRectangle(cornerRadius: 22, style: .continuous)
                            .fill(PocketTheme.field)
                    }
                    .disabled(agent.isRunning)

                Button {
                    let task = input
                    input = ""
                    agent.send(task)
                } label: {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(PocketTheme.onAccent)
                        .frame(width: 44, height: 44)
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
                    HStack(spacing: PocketSpacing.xs) {
                        Image(systemName: settings.thinkingEnabled ? "brain.head.profile" : "brain")
                        Text("Thinking")
                    }
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(settings.thinkingEnabled ? PocketTheme.accent : PocketTheme.secondaryText)
                    .padding(.vertical, PocketSpacing.sm)
                    .contentShape(Rectangle())
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
                // A filled bubble, the way a chat app marks what the person said, rather than
                // a tinted outline on the canvas.
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .fill(PocketTheme.userBubble)
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
                    // An assistant reply is prose and gets rendered as Markdown; anything else
                    // here is machine output and stays in a code font, where its own form is
                    // the point.
                    if event.kind == .assistant {
                        MarkdownText(source: event.detail)
                            .padding(.top, 2)
                    } else {
                        Text(event.detail)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(PocketTheme.primaryText.opacity(0.92))
                            .lineLimit(12)
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
        }
        .padding(.vertical, PocketSpacing.xs)
        .background {
            // No card for an assistant reply: text on the canvas, with the icon and name above
            // it. A bordered box around every answer is what made the transcript read as a list
            // of events rather than a conversation.
            if event.kind != .assistant {
                RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                    .fill(PocketTheme.surface)
            }
        }
        .overlay {
            if event.kind != .assistant {
                RoundedRectangle(cornerRadius: PocketRadius.lg, style: .continuous)
                    .stroke(PocketTheme.stroke.opacity(0.7), lineWidth: 1)
            }
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
