import EventKit
import Foundation
import LuaRuntime
import PocketDSHCore

@MainActor
final class ToolExecutor {
    private let workspace: WorkspaceFileSystem
    private let luaEngine: LuaEngine
    private let pluginRuntime: PluginRuntimeManager?
    private let systemActions: SystemActionStore
    private let eventStore = EKEventStore()

    init(
        workspace: WorkspaceFileSystem,
        luaEngine: LuaEngine,
        pluginRuntime: PluginRuntimeManager? = nil,
        systemActions: SystemActionStore
    ) {
        self.workspace = workspace
        self.luaEngine = luaEngine
        self.pluginRuntime = pluginRuntime
        self.systemActions = systemActions
    }

    func execute(
        name: String,
        argumentsJSON: String
    ) async -> ToolExecutionResult {
        if Task.isCancelled {
            return .failure("Cancelled before tool execution.")
        }

        if let pluginRuntime {
            if let pluginResult = await pluginRuntime.invoke(
                toolName: name,
                argumentsJSON: argumentsJSON
            ) {
                return pluginResult
            }
        }

        guard let arguments = parseArguments(argumentsJSON) else {
            return .failure("Tool arguments must be a JSON object.")
        }

        do {
            switch name {
            case ToolNames.listFiles:
                return try listFiles(arguments)
            case ToolNames.readFile:
                return try readFile(arguments)
            case ToolNames.writeFile:
                return try writeFile(arguments)
            case ToolNames.searchFiles:
                return try searchFiles(arguments)
            case ToolNames.applyPatch:
                return try applyPatch(arguments)
            case ToolNames.runLua:
                return try await runLua(arguments)
            case ToolNames.scheduleReminder:
                return try await scheduleReminder(arguments)
            case ToolNames.prepareMessage:
                return try prepareMessage(arguments)
            case ToolNames.prepareCall:
                return try prepareCall(arguments)
            default:
                return .failure("Unknown tool: \(name)")
            }
        } catch {
            return .failure(error.localizedDescription)
        }
    }

    private func listFiles(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        let path = arguments["path"]?.stringValue ?? "."
        let depth = min(max(arguments["depth"]?.intValue ?? 2, 1), 6)
        let includeHidden = arguments["include_hidden"]?.boolValue ?? false
        let entries = try workspace.listFiles(
            path: path,
            maxDepth: depth,
            includeHidden: includeHidden
        )

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(entries)
        return .success(String(decoding: data, as: UTF8.self))
    }

    private func readFile(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let path = arguments["path"]?.stringValue else {
            return .failure("read_file requires path.")
        }
        let offset = max(arguments["offset"]?.intValue ?? 1, 1)
        let limit = min(max(arguments["limit"]?.intValue ?? 400, 1), 2_000)
        let result = try workspace.read(path: path, offset: offset, limit: limit)

        var output = "path: \(result.path)\n"
        output += "range: \(result.startLine)-\(result.endLine) of \(result.totalLines)\n"
        if result.truncated {
            output += "note: truncated; request a later offset for more.\n"
        }
        output += "-----\n"
        if result.lines.isEmpty {
            output += "(no lines in range)\n"
        } else {
            for (index, line) in result.lines.enumerated() {
                output += "\(result.startLine + index)\t\(line)\n"
            }
        }
        return .success(output)
    }

    private func writeFile(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let path = arguments["path"]?.stringValue else {
            return .failure("write_file requires path.")
        }
        guard let content = arguments["content"]?.stringValue else {
            return .failure("write_file requires content.")
        }
        let overwrite = arguments["overwrite"]?.boolValue ?? false
        let result = try workspace.write(
            path: path,
            content: content,
            overwrite: overwrite
        )

        var output = "\(result.created ? "Created" : "Updated") \(result.path) (\(result.bytesWritten) bytes).\n"
        if let backup = result.backupPath {
            output += "backup: \(backup)\n"
        }
        output += "diff (truncated):\n"
        output += String(result.diff.prefix(8_000))
        if result.diff.count > 8_000 {
            output += "\n... diff truncated ..."
        }
        return .success(output)
    }

    private func searchFiles(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let query = arguments["query"]?.stringValue else {
            return .failure("search_files requires query.")
        }
        let path = arguments["path"]?.stringValue ?? "."
        let include = arguments["include"]?.stringValue
        let maxResults = min(max(arguments["max_results"]?.intValue ?? 100, 1), 500)
        let matches = try workspace.search(
            query: query,
            path: path,
            include: include,
            maxResults: maxResults
        )

        guard !matches.isEmpty else {
            return .success("No matches.")
        }
        return .success(
            matches.map { "\($0.path):\($0.line): \($0.preview)" }.joined(separator: "\n")
        )
    }

    private func applyPatch(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let patch = arguments["patch"]?.stringValue else {
            return .failure("apply_patch requires patch.")
        }
        let result = try workspace.applyPatch(patch)
        guard !result.changedPaths.isEmpty else {
            return .failure("Patch produced no changes.")
        }

        var output = "changed: \(result.changedPaths.joined(separator: ", "))\n"
        for path in result.changedPaths {
            output += "\n--- \(path) ---\n"
            let diff = result.diffs[path] ?? ""
            output += String(diff.prefix(6_000))
            if diff.count > 6_000 {
                output += "\n... diff truncated ..."
            }
            output += "\n"
        }
        return .success(output)
    }

    private func runLua(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let code = arguments["code"]?.stringValue else {
            return .failure("run_lua requires code.")
        }
        let requested = arguments["timeout_seconds"]?.intValue ?? 5
        let timeout = min(max(requested, 1), 10)
        let result = await luaEngine.run(code, timeout: TimeInterval(timeout))

        var output = "exit_code: \(result.exitCode)\n"
        output += "duration_ms: \(Int((result.duration * 1_000).rounded()))\n"
        output += "stdout:\n\(result.stdout)\n"
        output += "stderr:\n\(result.stderr)"
        return result.succeeded ? .success(output) : .failure(output)
    }


    private func scheduleReminder(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let title = arguments["title"]?.stringValue else {
            return .failure("schedule_reminder requires title.")
        }
        guard let due = arguments["due_iso8601"]?.stringValue,
              let date = ISO8601DateFormatter().date(from: due) else {
            return .failure("schedule_reminder requires a valid due_iso8601 timestamp.")
        }

        let granted: Bool
        do {
            granted = try await eventStore.requestFullAccessToReminders()
        } catch {
            return .failure("Reminder permission failed: \(error.localizedDescription)")
        }
        guard granted else {
            return .failure("Reminder permission was not granted. The user must allow Reminders access.")
        }
        guard let calendar = eventStore.defaultCalendarForNewReminders() else {
            return .failure("No default Reminders calendar is available.")
        }

        let reminder = EKReminder(eventStore: eventStore)
        reminder.title = title
        reminder.notes = arguments["notes"]?.stringValue
        reminder.calendar = calendar
        reminder.dueDateComponents = Calendar.current.dateComponents(
            [.year, .month, .day, .hour, .minute],
            from: date
        )

        do {
            try eventStore.save(reminder, commit: true)
        } catch {
            return .failure("Could not save reminder: \(error.localizedDescription)")
        }
        return .success("Created a real reminder in the user's Reminders app for \(date.formatted()). This is a reminder, not a Clock app alarm.")
    }

    private func prepareMessage(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let recipient = arguments["recipient"]?.stringValue,
              let body = arguments["body"]?.stringValue else {
            return .failure("prepare_message requires recipient and body.")
        }
        systemActions.prepareMessage(recipient: recipient, body: body)
        return .success("Prepared a message UI for the user. Nothing was sent. The user must tap Send in the system composer.")
    }

    private func prepareCall(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        guard let number = arguments["number"]?.stringValue else {
            return .failure("prepare_call requires number.")
        }
        systemActions.prepareCall(number: number)
        return .success("Prepared a phone call for the user. Nothing was dialed. The system will ask for confirmation.")
    }
    private func parseArguments(_ json: String) -> [String: JSONValue]? {
        guard let data = json.data(using: .utf8),
              let value = try? JSONDecoder().decode(JSONValue.self, from: data) else {
            return nil
        }
        return value.objectValue
    }
}
