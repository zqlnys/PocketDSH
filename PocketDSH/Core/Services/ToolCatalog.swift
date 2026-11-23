import Foundation
import PocketDSHCore

enum ToolCatalog {
    static let specs: [LLMToolSpec] = [
        tool(
            name: ToolNames.listFiles,
            description: "List files and directories inside the active iPhone workspace. Paths are relative to the workspace root. Use this before guessing file locations.",
            properties: [
                "path": property("string", "Relative directory path, default ."),
                "depth": property("integer", "Recursive depth from 1 to 6, default 2"),
                "include_hidden": property("boolean", "Include dotfiles, default false")
            ],
            required: []
        ),
        tool(
            name: ToolNames.readFile,
            description: "Read a UTF-8 text file from the active workspace with line numbers. Use offset and limit for large files.",
            properties: [
                "path": property("string", "Relative file path"),
                "offset": property("integer", "First line to return, 1-based, default 1"),
                "limit": property("integer", "Maximum lines, default 400, max 2000")
            ],
            required: ["path"]
        ),
        tool(
            name: ToolNames.writeFile,
            description: "Create or replace a file in the active workspace. Existing files require overwrite=true. A backup and line diff are created automatically for existing files.",
            properties: [
                "path": property("string", "Relative file path"),
                "content": property("string", "Complete UTF-8 content"),
                "overwrite": property("boolean", "Set true only when replacing an existing file")
            ],
            required: ["path", "content", "overwrite"]
        ),
        tool(
            name: ToolNames.searchFiles,
            description: "Search text files in the active workspace for a case-insensitive query. By default the query is literal; set regex to true to use a regular expression. Optionally filter by a glob such as *.swift.",
            properties: [
                "query": property("string", "Text to find, or a regular expression when regex is true"),
                "path": property("string", "Relative directory or file path, default ."),
                "include": property("string", "Optional glob filter, for example *.swift"),
                "regex": property("boolean", "Treat query as a regular expression instead of a literal substring"),
                "max_results": property("integer", "Maximum matches, default 100, max 500")
            ],
            required: ["query"]
        ),
        tool(
            name: ToolNames.applyPatch,
            description: "Apply a unified diff to one or more existing UTF-8 files. Every hunk is verified before writing; backups and diffs are recorded. Create files with write_file first.",
            properties: [
                "patch": property("string", "Unified diff text with ---/+++ headers and @@ hunks")
            ],
            required: ["patch"]
        ),
        tool(
            name: ToolNames.scheduleAlert,
            description: "Schedule a local notification alert on this iPhone. This is an app notification, not a Clock app alarm. The system asks for notification permission.",
            properties: [
                "title": property("string", "Alert title"),
                "due_iso8601": property("string", "Fire time as an ISO-8601 timestamp"),
                "body": property("string", "Optional alert body")
            ],
            required: ["title", "due_iso8601"]
        ),
        tool(
            name: ToolNames.scheduleReminder,
            description: "Create a reminder in the user's Reminders app. This is a reminder, not a Clock app alarm. The system may ask for Reminders permission.",
            properties: [
                "title": property("string", "Reminder title"),
                "due_iso8601": property("string", "Due time as an ISO-8601 timestamp"),
                "notes": property("string", "Optional notes")
            ],
            required: ["title", "due_iso8601"]
        ),
        tool(
            name: ToolNames.prepareMessage,
            description: "Prepare a prefilled SMS/iMessage composer for the user. Nothing is sent automatically; the user must tap Send.",
            properties: [
                "recipient": property("string", "Phone number or contact"),
                "body": property("string", "Message body")
            ],
            required: ["recipient", "body"]
        ),
        tool(
            name: ToolNames.prepareCall,
            description: "Prepare a phone call confirmation for the user. Nothing is dialed automatically; the system asks before calling.",
            properties: [
                "number": property("string", "Phone number")
            ],
            required: ["number"]
        ),
        tool(
            name: ToolNames.useSkill,
            description: "Skills are markdown files under skills/ in the workspace that explain how to carry out a kind of task. Call with no arguments to list what is available; call with a name to load that skill's full text. Load a skill before starting work it covers, and follow what it says.",
            properties: [
                "name": property("string", "Skill name, without the .md suffix. Omit to list available skills.")
            ],
            required: []
        ),
        tool(
            name: ToolNames.present,
            description: "Mark a file in the workspace as a finished artifact for the user to open. The path must already exist; this does not create files. Use it when a task produces something worth looking at, not for every file you touch. Calling it again with the same path updates the summary instead of adding a duplicate.",
            properties: [
                "path": property("string", "Relative path of an existing workspace file"),
                "summary": property("string", "One line saying what it is or why it matters")
            ],
            required: ["path", "summary"]
        ),
        tool(
            name: ToolNames.todoWrite,
            description: "Replace the plan for this task. Send the complete list each time, with every step's current status. Use it for work that needs more than two or three steps so progress is visible; keep each entry to one short line. Exactly one step may be in_progress at a time.",
            properties: [
                "todos": .object([
                    "type": .string("array"),
                    "description": .string("Complete list, in order."),
                    "items": .object([
                        "type": .string("object"),
                        "properties": .object([
                            "content": .object(["type": .string("string")]),
                            "status": .object([
                                "type": .string("string"),
                                "enum": .array([.string("pending"),
                                                .string("in_progress"),
                                                .string("completed")])
                            ])
                        ]),
                        "required": .array([.string("content"), .string("status")])
                    ])
                ])
            ],
            required: ["todos"]
        ),
        tool(
            name: ToolNames.fetchURL,
            description: "Fetch a web page or raw text resource over http(s) and return its text. HTML is reduced to readable text. Use this for documentation the workspace does not contain; do not use it to download files into the workspace.",
            properties: [
                "url": property("string", "Absolute http or https URL"),
                "max_characters": property("integer", "Truncate the returned text to this many characters, default 20000, max 200000")
            ],
            required: ["url"]
        ),
        tool(
            name: ToolNames.webSearch,
            description: "Search the web and return the top results as a numbered list of title, URL and snippet. Use it to find pages worth reading, then fetch_url the one you need. It returns links, not page contents.",
            properties: [
                "query": property("string", "What to search for"),
                "count": property("integer", "How many results to return, default 5, max 10")
            ],
            required: ["query"]
        ),
        tool(
            name: ToolNames.subagent,
            description: "Run a self-contained sub-task in its own conversation and return only its final answer. The sub-agent cannot see this conversation, so state everything it needs in the prompt. Use it to keep intermediate reasoning out of the main context.",
            properties: [
                "prompt": property("string", "A complete, self-contained instruction for the sub-agent"),
                "label": property("string", "Short name for this sub-task, shown in the timeline")
            ],
            required: ["prompt"]
        ),
        tool(
            name: ToolNames.runLinux,
            description: "Run one shell command in a real Alpine Linux rootfs, interpreted on the device with no JIT and no subprocess. /bin/sh, the busybox applets, and CPython 3.12 with its standard library are available. The project is mounted at /workspace, so commands read and write the same files the file tools do. Output and exit status are returned, and a non-zero exit is reported as a failure with its output. Each call starts a fresh rootfs session, so nothing persists between calls except what reaches /workspace.",
            properties: [
                "command": property("string", "Shell command to run in Alpine Linux")
            ],
            required: ["command"]
        ),
        tool(
            name: ToolNames.runLua,
            description: "Execute Lua 5.4 code locally on the iPhone in a sandbox. Returns real stdout, stderr and exit code. No shell, process launching, network, io, os, package or debug libraries are available.",
            properties: [
                "code": property("string", "Lua 5.4 source code"),
                "timeout_seconds": property("number", "Wall-clock limit from 1 to 10 seconds, default 5")
            ],
            required: ["code"]
        )
    ]

    static func systemPrompt(projectName: String) -> String {
        """
        You are PocketDSH, a coding agent running entirely on the user's iPhone. The active project is "\(projectName)". The phone, not a remote server, is the workspace.

        Tools operate on real files in the active workspace. Never claim a file was changed or a test passed unless the tool result proves it. Paths passed to tools must be relative to the workspace root; absolute paths and parent traversal are rejected.

        Work in small, verifiable steps:
        1. Inspect the tree and read the relevant files before editing.
        2. Prefer apply_patch for targeted multi-line edits and write_file for new files or complete replacements.
        3. Read the changed file again after editing when correctness matters.
        4. Use run_lua for local computation and executable checks. Lua is available; shell commands, Node, Python, Xcode, and subprocesses are not in this build.
        5. For work that needs more than two or three steps, record the plan with todo_write before starting and update it as steps finish. Send the whole list each time, keep exactly one step in_progress, and keep entries to one short line. Skip it for single-step requests.
        6. Call use_skill with no arguments to see what skills this workspace defines, and load the one that matches the task before starting. Skills are written by the user and are instructions, not data to summarise.
        7. When a task produces a file worth opening, mark it with present so it appears as a card above the conversation. Present files that already exist; the tool does not create them.
        8. Use fetch_url to read documentation the workspace does not contain. It performs a plain GET and reduces HTML to text; it cannot write to the workspace, and results are truncated, so prefer the file tools for anything already on the device.
        9. Use web_search when you do not know which page holds the answer. It returns titles, URLs and snippets; follow up with fetch_url on the one worth reading. It reaches the public web only - nothing on this device and nothing behind a login.
        10. Use subagent for a self-contained piece of work whose intermediate steps you do not need to see - a broad search, an independent check, a focused edit. It runs with its own conversation and returns only the final answer, so the parent context stays small. Give it everything it needs: it cannot see this conversation.
        11. Report changed paths, real tool results, and anything you could not verify.

        Existing files require overwrite=true in write_file. Every overwrite creates a recovery backup and a diff. Never request a destructive operation just to make a build pass. If a tool fails, inspect the error and try a different safe approach. Do not invent tool output.

        Extra tools named plugin__* come from installed local Lua plugins. They are real tools with declared permissions and must be treated like the built-in tools: read their result, do not claim success without it.
        """
    }

    private static func tool(
        name: String,
        description: String,
        properties: [String: JSONValue],
        required: [String]
    ) -> LLMToolSpec {
        let parameters: JSONValue = .object([
            "type": .string("object"),
            "properties": .object(properties),
            "required": .array(required.map(JSONValue.string)),
            "additionalProperties": .bool(false)
        ])
        return LLMToolSpec(
            function: .init(
                name: name,
                description: description,
                parameters: parameters
            )
        )
    }

    private static func property(_ type: String, _ description: String) -> JSONValue {
        .object([
            "type": .string(type),
            "description": .string(description)
        ])
    }
}
