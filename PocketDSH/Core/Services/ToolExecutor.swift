import EventKit
import Foundation
import UserNotifications
import LuaRuntime
import PocketDSHCore

@MainActor
final class ToolExecutor {
    private let workspace: WorkspaceFileSystem
    private let luaEngine: LuaEngine
    private let pluginRuntime: PluginRuntimeManager?
    private let systemActions: SystemActionStore
    /// The plan the model last submitted, mirrored into AgentRunner so the UI can
    /// show progress without scraping conversation text.
    private(set) var todos: [TodoItem] = []
    var onTodosChanged: (([TodoItem]) -> Void)?
    /// Files marked with `present`, mirrored into AgentRunner for the card strip.
    private(set) var deliverables: [Deliverable] = []
    var onDeliverablesChanged: (([Deliverable]) -> Void)?

    private let linuxRuntime: LinuxRuntime?
    private let eventStore = EKEventStore()

    init(
        workspace: WorkspaceFileSystem,
        luaEngine: LuaEngine,
        pluginRuntime: PluginRuntimeManager? = nil,
        systemActions: SystemActionStore,
        linuxRuntime: LinuxRuntime? = nil
    ) {
        self.workspace = workspace
        self.luaEngine = luaEngine
        self.pluginRuntime = pluginRuntime
        self.systemActions = systemActions
        self.linuxRuntime = linuxRuntime
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
            case ToolNames.runLinux:
                return try await runLinux(arguments)
            case ToolNames.scheduleReminder:
                return try await scheduleReminder(arguments)
            case ToolNames.scheduleAlert:
                return try await scheduleAlert(arguments)
            case ToolNames.prepareMessage:
                return try prepareMessage(arguments)
            case ToolNames.prepareCall:
                return try prepareCall(arguments)
            case ToolNames.fetchURL:
                return try await fetchURL(arguments)
            case ToolNames.webSearch:
                return try await webSearch(arguments)
            case ToolNames.todoWrite:
                return todoWrite(arguments)
            case ToolNames.present:
                return present(arguments)
            case ToolNames.useSkill:
                return try useSkill(arguments)
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
        let useRegex = arguments["regex"]?.boolValue ?? false
        let matches = try workspace.search(
            query: query,
            path: path,
            include: include,
            maxResults: maxResults,
            useRegex: useRegex
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




    private func runLinux(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let command = arguments["command"]?.stringValue, !command.isEmpty else {
            return .failure("run_linux requires command.")
        }
        guard let linuxRuntime else {
            return .failure("Tier C is not linked in this build.")
        }
        // The active project root is mounted at /workspace via iSH realfs, so a
        // guest command sees the same files the Swift tools operate on.
        let result = await linuxRuntime.run(command: command, workspace: workspace.root)
        guard !result.isError else { return result }
        return .success("Linux output (Tier C):\n\(result.content)")
    }
    /// List or load a skill.
    ///
    /// A skill is a markdown file under skills/ that tells the agent how to do
    /// a kind of task. Listing and loading are one tool because the model
    /// otherwise has to guess whether the directory exists at all, and a guess
    /// that is wrong wastes a whole turn.
    private func useSkill(_ arguments: [String: JSONValue]) throws -> ToolExecutionResult {
        let requested = (arguments["name"]?.stringValue ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        // A workspace with no skills/ directory is the normal case, not an
        // error: listing must not throw just because nobody has written one.
        let entries = (try? workspace.listFiles(path: "skills", maxDepth: 1,
                                                includeHidden: false)) ?? []
        let skills = entries.filter { !$0.isDirectory && $0.path.hasSuffix(".md") }

        if requested.isEmpty {
            guard !skills.isEmpty else {
                return .success("No skills are defined. Create skills/<name>.md in the workspace to add one.")
            }
            var output = "Available skills:\n"
            for entry in skills {
                let name = Self.skillName(from: entry.path)
                // The first non-empty line is the description; skills are
                // written to start with one.
                let preview = (try? workspace.read(path: entry.path, offset: 1, limit: 1))?
                    .lines.first ?? ""
                output += "  \(name)"
                if !preview.isEmpty {
                    output += " - \(preview.trimmingCharacters(in: .whitespaces))"
                }
                output += "\n"
            }
            return .success(output)
        }

        let wanted = requested.hasSuffix(".md") ? requested : requested + ".md"
        guard let entry = skills.first(where: { Self.skillName(from: $0.path) == requested
            || $0.path.hasSuffix("/" + wanted) || $0.path == wanted || $0.path == "skills/" + wanted })
        else {
            return .failure("No skill named \(requested). Call use_skill with no arguments to list them.")
        }
        let result = try workspace.read(path: entry.path, offset: 1, limit: 2_000)
        var output = "skill: \(Self.skillName(from: entry.path))\n"
        if result.truncated {
            output += "note: truncated at 2000 lines.\n"
        }
        output += "-----\n"
        output += result.lines.joined(separator: "\n")
        return .success(output)
    }

    private static func skillName(from path: String) -> String {
        var name = path
        if name.hasPrefix("skills/") {
            name = String(name.dropFirst("skills/".count))
        }
        if name.hasSuffix(".md") {
            name = String(name.dropLast(3))
        }
        return name
    }

    /// Declare a finished artifact.
    ///
    /// The file has to exist. `present` says "this is worth opening", not "here
    /// is a file" - creating it is write_file's job, and letting present succeed
    /// on a path that is not there would put a card in front of the user that
    /// opens nothing.
    private func present(_ arguments: [String: JSONValue]) -> ToolExecutionResult {
        guard let rawPath = arguments["path"]?.stringValue else {
            return .failure("present requires path.")
        }
        let path = rawPath.trimmingCharacters(in: .whitespacesAndNewlines)
        if path.isEmpty {
            return .failure("present requires a non-empty path.")
        }
        let summary = (arguments["summary"]?.stringValue ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)

        let entries: [FileEntry]
        do {
            entries = try workspace.listFiles(path: ".", maxDepth: 6,
                                              includeHidden: true)
        } catch {
            return .failure("present could not read the workspace: \(error.localizedDescription)")
        }
        guard let entry = entries.first(where: { $0.path == path }) else {
            return .failure("present: no such workspace file \(path). Create it with write_file first.")
        }
        if entry.isDirectory {
            return .failure("present takes a file, not a directory: \(path)")
        }

        var items = deliverables
        if let index = items.firstIndex(where: { $0.path == path }) {
            items[index].summary = summary.isEmpty ? items[index].summary : summary
        } else {
            items.append(Deliverable(path: path,
                                     summary: String(summary.prefix(200))))
        }
        deliverables = items
        onDeliverablesChanged?(items)

        var output = "presented \(items.count) file(s):\n"
        for item in items {
            output += "  \(item.path)"
            if !item.summary.isEmpty {
                output += " - \(item.summary)"
            }
            output += "\n"
        }
        return .success(output)
    }

    /// Adopt a plan restored from disk, without treating it as a change.
    func restoreTodos(_ items: [TodoItem]) {
        todos = items
    }

    /// Adopt deliverables restored from disk, without treating them as a change.
    func restoreDeliverables(_ items: [Deliverable]) {
        deliverables = items
    }

    /// Replace the plan. The model sends the whole list every time, which keeps
    /// the tool stateless from its side and makes the result the single source
    /// of truth for what is done.
    private func todoWrite(_ arguments: [String: JSONValue]) -> ToolExecutionResult {
        guard let raw = arguments["todos"]?.arrayValue else {
            return .failure("todo_write requires todos to be an array.")
        }
        if raw.count > 50 {
            return .failure("todo_write accepts at most 50 entries.")
        }
        var items: [TodoItem] = []
        var inProgress = 0
        for (index, entry) in raw.enumerated() {
            guard let object = entry.objectValue else {
                return .failure("todo_write entry \(index + 1) is not an object.")
            }
            let content = (object["content"]?.stringValue ?? "")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if content.isEmpty {
                return .failure("todo_write entry \(index + 1) has empty content.")
            }
            let statusText = object["status"]?.stringValue ?? TodoStatus.pending.rawValue
            guard let status = TodoStatus(rawValue: statusText) else {
                return .failure("todo_write entry \(index + 1) has an unknown status \(statusText).")
            }
            if status == .inProgress { inProgress += 1 }
            items.append(TodoItem(content: String(content.prefix(160)), status: status))
        }
        if inProgress > 1 {
            return .failure("todo_write allows one in_progress entry; \(inProgress) were given.")
        }
        // Keep ids stable when a step is merely re-described, so the list does
        // not animate as if every row were new.
        for index in items.indices where index < todos.count {
            if todos[index].content == items[index].content {
                items[index] = TodoItem(id: todos[index].id,
                                        content: items[index].content,
                                        status: items[index].status)
            }
        }
        todos = items
        onTodosChanged?(items)

        let done = items.filter { $0.status == .completed }.count
        var output = "plan: \(done)/\(items.count) completed\n"
        for item in items {
            let marker: String
            switch item.status {
            case .pending: marker = "[ ]"
            case .inProgress: marker = "[~]"
            case .completed: marker = "[x]"
            }
            output += "\(marker) \(item.content)\n"
        }
        return .success(output)
    }

    /// Fetch a URL and return readable text.
    ///
    /// The desktop harness hands this job to a separate fetch layer; a phone can
    /// do a plain GET, which is what reading documentation needs. HTML is
    /// reduced to text here rather than returned raw, because the model spends
    /// its whole context on markup otherwise.
    /// A search that needs no API key.
    ///
    /// The desktop tool talks to a search API; this app has exactly one
    /// credential and it belongs to the model, so the HTML endpoint is the only
    /// option. The result list is small and the format stable enough to parse,
    /// and the parser is written to degrade to "no results" rather than to
    /// return something malformed.
    private func webSearch(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let query = arguments["query"]?.stringValue,
              !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return .failure("web_search requires a non-empty query.")
        }
        let count = min(max(arguments["count"]?.intValue ?? 5, 1), 10)

        var components = URLComponents(string: "https://html.duckduckgo.com/html/")!
        components.queryItems = [URLQueryItem(name: "q", value: query)]
        guard let url = components.url else {
            return .failure("web_search could not build a request for that query.")
        }

        var request = URLRequest(url: url)
        request.timeoutInterval = 20
        request.httpMethod = "GET"
        request.setValue("PocketDSH/1.0 (iOS; local agent)", forHTTPHeaderField: "User-Agent")
        request.setValue("text/html", forHTTPHeaderField: "Accept")

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch {
            return .failure("web_search failed: \(error.localizedDescription)")
        }
        if Task.isCancelled {
            return .failure("Cancelled before reading the response.")
        }

        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        let body = String(data: data, encoding: .utf8) ?? ""
        let results = Self.searchResults(fromHTML: body, limit: count)

        if results.isEmpty {
            return .success("query: \(query)\nstatus: \(status)\nresults: 0\n-----\nNo results were parsed. The page may be a captcha, a consent screen, or a layout change.")
        }
        var output = "query: \(query)\nstatus: \(status)\nresults: \(results.count)\n-----\n"
        for (index, item) in results.enumerated() {
            output += "\(index + 1). \(item.title)\n   \(item.url)\n"
            if !item.snippet.isEmpty {
                output += "   \(item.snippet)\n"
            }
        }
        return .success(output)
    }

    struct SearchHit: Equatable {
        let title: String
        let url: String
        let snippet: String
    }

    /// Pull title, target URL and snippet out of a results page.
    ///
    /// Deliberately hand-rolled rather than a DOM parse: the page is fetched
    /// once, the shape is stable, and a dependency that can fail to build is a
    /// worse trade than eighty lines here. Anything unrecognised yields no
    /// result rather than a wrong one.
    nonisolated static func searchResults(fromHTML html: String, limit: Int) -> [SearchHit] {
        var hits: [SearchHit] = []
        var cursor = html.startIndex
        let anchor = "class=\"result__a\""

        while hits.count < limit {
            guard let marker = html.range(of: anchor, range: cursor..<html.endIndex) else { break }
            // The href may precede or follow the class attribute; take the
            // nearest one inside this tag.
            let tagStart = html.range(of: "<a ", options: .backwards,
                                      range: cursor..<marker.lowerBound)?.lowerBound
                ?? marker.lowerBound
            guard let tagEnd = html.range(of: ">", range: marker.upperBound..<html.endIndex) else { break }
            let tag = String(html[tagStart..<tagEnd.lowerBound])

            guard let hrefValue = attribute("href", in: tag),
                  let target = expandRedirect(hrefValue) else {
                cursor = tagEnd.upperBound
                continue
            }
            guard let close = html.range(of: "</a>", range: tagEnd.upperBound..<html.endIndex) else { break }
            let title = Self.plainText(fromHTML: String(html[tagEnd.upperBound..<close.lowerBound]))
                .trimmingCharacters(in: .whitespacesAndNewlines)

            var snippet = ""
            let afterLink = close.upperBound
            if let snippetMarker = html.range(of: "class=\"result__snippet\"",
                                              range: afterLink..<html.endIndex),
               let snippetOpen = html.range(of: ">", range: snippetMarker.upperBound..<html.endIndex),
               let snippetClose = html.range(of: "</a>", range: snippetOpen.upperBound..<html.endIndex) {
                snippet = Self.plainText(fromHTML: String(html[snippetOpen.upperBound..<snippetClose.lowerBound]))
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                cursor = snippetClose.upperBound
            } else {
                cursor = afterLink
            }
            if !title.isEmpty {
                hits.append(SearchHit(title: title, url: target, snippet: snippet))
            }
        }
        return hits
    }

    private nonisolated static func attribute(_ name: String, in tag: String) -> String? {
        guard let key = tag.range(of: "\(name)=\"") else { return nil }
        let start = key.upperBound
        guard let end = tag.range(of: "\"", range: start..<tag.endIndex) else { return nil }
        return String(tag[start..<end.lowerBound])
            .replacingOccurrences(of: "&amp;", with: "&")
    }

    /// DuckDuckGo wraps targets in a redirect of the form `.../l/?uddg=<url>`.
    /// Unwrapping it means the tool returns the address a person could paste,
    /// not a tracking link.
    private nonisolated static func expandRedirect(_ href: String) -> String? {
        let absolute: String
        if href.hasPrefix("//") {
            absolute = "https:" + href
        } else if href.hasPrefix("/") {
            absolute = "https://duckduckgo.com" + href
        } else if href.hasPrefix("http") {
            absolute = href
        } else {
            return nil
        }
        guard let components = URLComponents(string: absolute),
              let raw = components.queryItems?.first(where: { $0.name == "uddg" })?.value else {
            return absolute
        }
        return raw.hasPrefix("http") ? raw : absolute
    }

    private func fetchURL(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let raw = arguments["url"]?.stringValue else {
            return .failure("fetch_url requires url.")
        }
        guard let url = URL(string: raw),
              let scheme = url.scheme?.lowercased(),
              scheme == "http" || scheme == "https" else {
            return .failure("fetch_url needs an absolute http or https URL.")
        }
        let limit = min(max(arguments["max_characters"]?.intValue ?? 20_000, 1_000), 200_000)

        var request = URLRequest(url: url)
        request.timeoutInterval = 20
        request.httpMethod = "GET"
        request.setValue("PocketDSH/1.0 (iOS; local agent)", forHTTPHeaderField: "User-Agent")
        request.setValue("text/html,text/plain;q=0.9,*/*;q=0.5", forHTTPHeaderField: "Accept")

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch {
            return .failure("fetch_url failed: \(error.localizedDescription)")
        }
        if Task.isCancelled {
            return .failure("Cancelled before reading the response.")
        }

        let http = response as? HTTPURLResponse
        let status = http?.statusCode ?? 0
        let mime = http?.mimeType ?? "unknown"
        let body = String(data: data, encoding: .utf8)
            ?? String(data: data, encoding: .isoLatin1)
            ?? ""

        let text = mime.contains("html") ? Self.plainText(fromHTML: body) : body
        let clipped = text.count > limit ? String(text.prefix(limit)) : text

        var output = "url: \(url.absoluteString)\n"
        output += "status: \(status)\n"
        output += "content_type: \(mime)\n"
        output += "bytes_received: \(data.count)\n"
        output += "characters: \(text.count)\n"
        if text.count > limit {
            output += "note: truncated to \(limit) characters.\n"
        }
        output += "-----\n"
        output += clipped
        return .success(output)
    }

    /// Reduce HTML to readable text: drop script and style bodies entirely, turn
    /// remaining tags into spaces so words do not run together, and collapse the
    /// whitespace that leaves behind.
    nonisolated static func plainText(fromHTML html: String) -> String {
        var text = html
        text = text.replacingOccurrences(
            of: "(?is)<script.*?</script>", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(
            of: "(?is)<style.*?</style>", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(
            of: "(?s)<!--.*?-->", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(
            of: "(?s)<[^>]+>", with: " ", options: .regularExpression)
        for (entity, replacement) in [("&nbsp;", " "), ("&amp;", "&"), ("&lt;", "<"),
                                      ("&gt;", ">"), ("&quot;", "\""), ("&#39;", "'")] {
            text = text.replacingOccurrences(of: entity, with: replacement)
        }
        text = text.replacingOccurrences(
            of: "[ \t]+", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(
            of: " ?\n ?", with: "\n", options: .regularExpression)
        text = text.replacingOccurrences(
            of: "\n{3,}", with: "\n\n", options: .regularExpression)
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func scheduleAlert(_ arguments: [String: JSONValue]) async throws -> ToolExecutionResult {
        guard let title = arguments["title"]?.stringValue else {
            return .failure("schedule_alert requires title.")
        }
        guard let due = arguments["due_iso8601"]?.stringValue,
              let date = ISO8601DateFormatter().date(from: due),
              date > Date() else {
            return .failure("schedule_alert requires a future due_iso8601 timestamp.")
        }

        let center = UNUserNotificationCenter.current()
        let granted: Bool
        do {
            granted = try await center.requestAuthorization(options: [.alert, .sound])
        } catch {
            return .failure("Notification permission failed: \(error.localizedDescription)")
        }
        guard granted else {
            return .failure("Notification permission was not granted.")
        }

        let content = UNMutableNotificationContent()
        content.title = title
        content.body = arguments["body"]?.stringValue ?? ""
        content.sound = .default
        let components = Calendar.current.dateComponents(
            [.year, .month, .day, .hour, .minute],
            from: date
        )
        let trigger = UNCalendarNotificationTrigger(dateMatching: components, repeats: false)
        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: trigger
        )

        do {
            try await center.add(request)
        } catch {
            return .failure("Could not schedule notification: \(error.localizedDescription)")
        }
        return .success("Scheduled a local notification for \(date.formatted()). This is an app notification, not a Clock app alarm.")
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
