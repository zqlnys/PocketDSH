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
            description: "Search text files in the active workspace for a literal case-insensitive query. Optionally filter by a glob such as *.swift.",
            properties: [
                "query": property("string", "Literal text to find"),
                "path": property("string", "Relative directory or file path, default ."),
                "include": property("string", "Optional glob filter, for example *.swift"),
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
            name: ToolNames.fetchURL,
            description: "Fetch a web page or raw text resource over http(s) and return its text. HTML is reduced to readable text. Use this for documentation the workspace does not contain; do not use it to download files into the workspace.",
            properties: [
                "url": property("string", "Absolute http or https URL"),
                "max_characters": property("integer", "Truncate the returned text to this many characters, default 20000, max 200000")
            ],
            required: ["url"]
        ),
        tool(
            name: ToolNames.runLinux,
            description: "Experimental Tier C: run one command in the bundled Alpine Linux rootfs through the embedded iSH interpreter. No JIT. It cannot see the project workspace yet; use file tools for project files. One command per app launch.",
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
        5. Use fetch_url to read documentation the workspace does not contain. It performs a plain GET and reduces HTML to text; it cannot write to the workspace, and results are truncated, so prefer the file tools for anything already on the device.
        6. Report changed paths, real tool results, and anything you could not verify.

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
