import Foundation

public struct LLMToolCall: Codable, Hashable, Sendable, Identifiable {
    public struct Function: Codable, Hashable, Sendable {
        public var name: String
        public var arguments: String

        public init(name: String, arguments: String) {
            self.name = name
            self.arguments = arguments
        }
    }

    public var id: String
    public var type: String
    public var function: Function

    public init(
        id: String,
        type: String = "function",
        function: Function
    ) {
        self.id = id
        self.type = type
        self.function = function
    }
}

public struct LLMToolSpec: Codable, Hashable, Sendable {
    public struct Function: Codable, Hashable, Sendable {
        public var name: String
        public var description: String
        public var parameters: JSONValue

        public init(name: String, description: String, parameters: JSONValue) {
            self.name = name
            self.description = description
            self.parameters = parameters
        }
    }

    public var type: String
    public var function: Function

    public init(function: Function) {
        self.type = "function"
        self.function = function
    }
}

public struct LLMMessage: Codable, Hashable, Sendable, Identifiable {
    public enum Role: String, Codable, Sendable {
        case system
        case user
        case assistant
        case tool
    }

    public var id: UUID
    public var role: Role
    public var content: String?
    public var toolCalls: [LLMToolCall]?
    public var toolCallID: String?
    public var name: String?

    public init(
        id: UUID = UUID(),
        role: Role,
        content: String? = nil,
        toolCalls: [LLMToolCall]? = nil,
        toolCallID: String? = nil,
        name: String? = nil
    ) {
        self.id = id
        self.role = role
        self.content = content
        self.toolCalls = toolCalls
        self.toolCallID = toolCallID
        self.name = name
    }

    enum CodingKeys: String, CodingKey {
        case role
        case content
        case toolCalls = "tool_calls"
        case toolCallID = "tool_call_id"
        case name
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = UUID()
        role = try container.decode(Role.self, forKey: .role)
        content = try container.decodeIfPresent(String.self, forKey: .content)
        toolCalls = try container.decodeIfPresent([LLMToolCall].self, forKey: .toolCalls)
        toolCallID = try container.decodeIfPresent(String.self, forKey: .toolCallID)
        name = try container.decodeIfPresent(String.self, forKey: .name)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(role, forKey: .role)
        try container.encodeIfPresent(content, forKey: .content)
        try container.encodeIfPresent(toolCalls, forKey: .toolCalls)
        try container.encodeIfPresent(toolCallID, forKey: .toolCallID)
        try container.encodeIfPresent(name, forKey: .name)
    }
}

public struct AgentEvent: Identifiable, Codable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case user
        case assistant
        case toolStarted
        case toolFinished
        case error
        case cancelled
        case status
    }

    public let id: UUID
    public let kind: Kind
    public let title: String
    public let detail: String
    public let timestamp: Date

    public init(
        id: UUID = UUID(),
        kind: Kind,
        title: String,
        detail: String = "",
        timestamp: Date = .now
    ) {
        self.id = id
        self.kind = kind
        self.title = title
        self.detail = detail
        self.timestamp = timestamp
    }
}

public enum TodoStatus: String, Codable, Hashable, Sendable {
    case pending
    case inProgress = "in_progress"
    case completed
}

/// One line of the agent's own plan. The desktop harness keeps a todo list for
/// multi-step work; this is the same idea, small enough to show above the
/// conversation on a phone.
public struct TodoItem: Codable, Hashable, Sendable, Identifiable {
    public let id: UUID
    public var content: String
    public var status: TodoStatus

    public init(id: UUID = UUID(), content: String, status: TodoStatus = .pending) {
        self.id = id
        self.content = content
        self.status = status
    }
}

/// A file the agent wants the user to look at.
///
/// The desktop harness has `present`, which turns a finished artifact into
/// something the user can open rather than a path buried in prose. This is the
/// same idea: a named workspace file plus one line saying why it matters.
public struct Deliverable: Codable, Hashable, Sendable, Identifiable {
    public let id: UUID
    public var path: String
    public var summary: String
    public var createdAt: Date

    public init(id: UUID = UUID(), path: String, summary: String,
                createdAt: Date = .now) {
        self.id = id
        self.path = path
        self.summary = summary
        self.createdAt = createdAt
    }
}

public struct AgentConversation: Codable, Sendable {
    public var messages: [LLMMessage]
    public var events: [AgentEvent]
    public var todos: [TodoItem]
    public var deliverables: [Deliverable]
    public var updatedAt: Date

    public init(
        messages: [LLMMessage] = [],
        events: [AgentEvent] = [],
        todos: [TodoItem] = [],
        deliverables: [Deliverable] = [],
        updatedAt: Date = .now
    ) {
        self.messages = messages
        self.events = events
        self.todos = todos
        self.deliverables = deliverables
        self.updatedAt = updatedAt
    }

    private enum CodingKeys: String, CodingKey {
        case messages, events, todos, deliverables, updatedAt
    }

    /// Conversations written before todos existed have no such key. Decoding
    /// them must keep working - losing someone's history to add a feature would
    /// be worse than not having the feature - so the field defaults to empty.
    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        messages = try container.decode([LLMMessage].self, forKey: .messages)
        events = try container.decode([AgentEvent].self, forKey: .events)
        todos = try container.decodeIfPresent([TodoItem].self, forKey: .todos) ?? []
        // `deliverables` postdates both of the others, so it goes through the
        // same tolerant path: older conversations must keep opening.
        deliverables = try container.decodeIfPresent([Deliverable].self,
                                                      forKey: .deliverables) ?? []
        updatedAt = try container.decode(Date.self, forKey: .updatedAt)
    }
}

public struct ToolExecutionResult: Sendable, Equatable {
    public let content: String
    public let isError: Bool

    public init(content: String, isError: Bool) {
        self.content = content
        self.isError = isError
    }

    public static func success(_ content: String) -> ToolExecutionResult {
        ToolExecutionResult(content: content, isError: false)
    }

    public static func failure(_ content: String) -> ToolExecutionResult {
        ToolExecutionResult(content: content, isError: true)
    }
}

public enum ToolNames {
    public static let listFiles = "list_files"
    public static let readFile = "read_file"
    public static let writeFile = "write_file"
    public static let searchFiles = "search_files"
    public static let applyPatch = "apply_patch"
    public static let runLua = "run_lua"
    public static let runLinux = "run_linux"
    public static let scheduleReminder = "schedule_reminder"
    public static let scheduleAlert = "schedule_alert"
    public static let prepareMessage = "prepare_message"
    public static let prepareCall = "prepare_call"
    /// Fetch a URL and return its text. The desktop harness has web_search and
    /// web_fetch; this is the part of that which a phone can do without a
    /// search API key.
    public static let fetchURL = "fetch_url"
    /// Searches the web and returns a small numbered list of results. Uses the
    /// HTML endpoint rather than an API, so it needs no key - which matters for
    /// an app whose only credential is the model key.
    public static let webSearch = "web_search"
    public static let todoWrite = "todo_write"
    /// Marks a workspace file as something the user should open.
    public static let present = "present"
    /// Loads a skill: a skill is a markdown file in the workspace that tells the
    /// agent how to do a kind of task. Listing and loading share one tool so the
    /// model does not have to guess whether the directory is empty.
    public static let useSkill = "use_skill"
}
