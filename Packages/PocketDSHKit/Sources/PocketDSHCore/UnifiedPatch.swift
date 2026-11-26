import Foundation

public struct PatchFileChange: Sendable, Equatable {
    public enum LineKind: Sendable, Equatable {
        case context
        case remove
        case add
    }

    public struct Line: Sendable, Equatable {
        public let kind: LineKind
        public let text: String

        public init(kind: LineKind, text: String) {
            self.kind = kind
            self.text = text
        }
    }

    public struct Hunk: Sendable, Equatable {
        public let oldStart: Int
        public let oldCount: Int
        public let newStart: Int
        public let newCount: Int
        public let lines: [Line]

        public init(
            oldStart: Int,
            oldCount: Int,
            newStart: Int,
            newCount: Int,
            lines: [Line]
        ) {
            self.oldStart = oldStart
            self.oldCount = oldCount
            self.newStart = newStart
            self.newCount = newCount
            self.lines = lines
        }
    }

    public let oldPath: String
    public let newPath: String
    public let hunks: [Hunk]

    public init(oldPath: String, newPath: String, hunks: [Hunk]) {
        self.oldPath = oldPath
        self.newPath = newPath
        self.hunks = hunks
    }

    public var path: String { newPath }
}

/// A line, short enough to sit in an error message.
private func shown(_ line: String, limit: Int = 120) -> String {
    line.count <= limit ? "\" \(line) \"" : "\" \(line.prefix(limit))… \""
}

public enum UnifiedPatch {
    public static func parse(_ patch: String) throws -> [PatchFileChange] {
        let rawLines = patch
            .replacingOccurrences(of: "\r\n", with: "\n")
            .components(separatedBy: "\n")
        var index = 0
        var changes: [PatchFileChange] = []

        while index < rawLines.count {
            let line = rawLines[index]
            if line.hasPrefix("--- ") {
                guard index + 1 < rawLines.count, rawLines[index + 1].hasPrefix("+++ ") else {
                    throw WorkspaceError.patchParse("Missing +++ header after \(line)")
                }
                let oldPath = try normalizedPath(String(line.dropFirst(4)))
                let newPath = try normalizedPath(String(rawLines[index + 1].dropFirst(4)))
                index += 2

                var hunks: [PatchFileChange.Hunk] = []
                while index < rawLines.count {
                    let candidate = rawLines[index]
                    if candidate.hasPrefix("--- ") {
                        break
                    }
                    guard candidate.hasPrefix("@@ ") else {
                        index += 1
                        continue
                    }

                    let header = try parseHunkHeader(candidate)
                    index += 1
                    var body: [PatchFileChange.Line] = []

                    while index < rawLines.count {
                        let bodyLine = rawLines[index]
                        if bodyLine.hasPrefix("@@ ") || bodyLine.hasPrefix("--- ") {
                            break
                        }
                        if bodyLine.hasPrefix("\\ No newline") {
                            index += 1
                            continue
                        }
                        if bodyLine.isEmpty {
                            index += 1
                            continue
                        }

                        switch bodyLine.first {
                        case " ":
                            body.append(.init(kind: .context, text: String(bodyLine.dropFirst())))
                        case "-":
                            body.append(.init(kind: .remove, text: String(bodyLine.dropFirst())))
                        case "+":
                            body.append(.init(kind: .add, text: String(bodyLine.dropFirst())))
                        default:
                            throw WorkspaceError.patchParse("Unexpected hunk line: \(bodyLine)")
                        }
                        index += 1
                    }

                    let actualOld = body.filter { $0.kind != .add }.count
                    let actualNew = body.filter { $0.kind != .remove }.count
                    guard actualOld == header.oldCount, actualNew == header.newCount else {
                        throw WorkspaceError.patchParse(
                            "Hunk count mismatch: header says -\(header.oldCount)/+\(header.newCount), body has -\(actualOld)/+\(actualNew)"
                        )
                    }

                    hunks.append(
                        .init(
                            oldStart: header.oldStart,
                            oldCount: header.oldCount,
                            newStart: header.newStart,
                            newCount: header.newCount,
                            lines: body
                        )
                    )
                }

                guard !hunks.isEmpty else {
                    throw WorkspaceError.patchParse("No hunks for \(newPath)")
                }
                changes.append(
                    .init(oldPath: oldPath, newPath: newPath, hunks: hunks)
                )
            } else {
                index += 1
            }
        }

        guard !changes.isEmpty else {
            throw WorkspaceError.patchParse("No file sections found")
        }
        return changes
    }

    /// Applies a parsed patch to in-memory UTF-8 contents. Callers must validate
    /// every returned path with `WorkspacePathGuard` before writing.
    public static func apply(
        _ changes: [PatchFileChange],
        to files: [String: String]
    ) throws -> [String: String] {
        var result = files

        for change in changes {
            guard !change.path.contains("/dev/null") else {
                throw WorkspaceError.patchParse("Creating or deleting files with /dev/null is not supported; use write_file.")
            }
            guard let original = files[change.oldPath] else {
                throw WorkspaceError.notFound(change.oldPath)
            }

            let originalLines = original.components(separatedBy: "\n")
            var output: [String] = []
            var cursor = 0

            for hunk in change.hunks {
                let start = hunk.oldStart == 0 ? 0 : hunk.oldStart - 1
                guard start >= cursor, start <= originalLines.count else {
                    throw WorkspaceError.patchMismatch(
                        "Hunk starts outside \(change.path) at line \(hunk.oldStart)"
                    )
                }

                if start > cursor {
                    output.append(contentsOf: originalLines[cursor..<start])
                    cursor = start
                }

                for line in hunk.lines {
                    switch line.kind {
                    case .context:
                        guard cursor < originalLines.count, originalLines[cursor] == line.text else {
                            // The line number says where, but not what - and what is what a patch
                            // has to be corrected against. Showing both turns a retry into an edit.
                            let found = cursor < originalLines.count
                                ? shown(originalLines[cursor]) : "end of file"
                            throw WorkspaceError.patchMismatch(
                                "Context mismatch in \(change.path) at line \(cursor + 1):\n"
                                    + "  expected: \(shown(line.text))\n"
                                    + "  found:    \(found)"
                            )
                        }
                        output.append(line.text)
                        cursor += 1
                    case .remove:
                        guard cursor < originalLines.count, originalLines[cursor] == line.text else {
                            let found = cursor < originalLines.count
                                ? shown(originalLines[cursor]) : "end of file"
                            throw WorkspaceError.patchMismatch(
                                "Removal mismatch in \(change.path) at line \(cursor + 1):\n"
                                    + "  expected: \(shown(line.text))\n"
                                    + "  found:    \(found)"
                            )
                        }
                        cursor += 1
                    case .add:
                        output.append(line.text)
                    }
                }
            }

            if cursor < originalLines.count {
                output.append(contentsOf: originalLines[cursor...])
            }

            result[change.path] = output.joined(separator: "\n")
            if change.oldPath != change.newPath {
                result.removeValue(forKey: change.oldPath)
            }
        }

        return result
    }

    private static func normalizedPath(_ raw: String) throws -> String {
        var path = raw.trimmingCharacters(in: .whitespaces)
        if path.hasPrefix("a/") || path.hasPrefix("b/") {
            path.removeFirst(2)
        }
        if path.hasPrefix("./") {
            path.removeFirst(2)
        }
        guard !path.isEmpty, !path.hasPrefix("/"), !path.contains("\0") else {
            throw WorkspaceError.patchParse("Invalid path in patch: \(raw)")
        }
        guard !path.split(separator: "/").contains("..") else {
            throw WorkspaceError.patchParse("Parent traversal in patch path: \(path)")
        }
        return path
    }

    private static func parseHunkHeader(
        _ line: String
    ) throws -> (oldStart: Int, oldCount: Int, newStart: Int, newCount: Int) {
        let parts = line.split(separator: " ", omittingEmptySubsequences: true)
        guard parts.count >= 3, parts[0] == "@@", parts[1].hasPrefix("-"), parts[2].hasPrefix("+") else {
            throw WorkspaceError.patchParse("Invalid hunk header: \(line)")
        }

        let old = try parseRange(parts[1], prefix: "-")
        let new = try parseRange(parts[2], prefix: "+")
        return (old.start, old.count, new.start, new.count)
    }

    private static func parseRange(
        _ token: Substring,
        prefix: Character
    ) throws -> (start: Int, count: Int) {
        guard token.first == prefix else {
            throw WorkspaceError.patchParse("Invalid range: \(token)")
        }
        let body = token.dropFirst()
        let parts = body.split(separator: ",", omittingEmptySubsequences: false)
        guard let start = Int(parts[0]), start >= 0 else {
            throw WorkspaceError.patchParse("Invalid range start: \(token)")
        }
        let count: Int
        if parts.count > 1 {
            guard let parsed = Int(parts[1]), parsed >= 0 else {
                throw WorkspaceError.patchParse("Invalid range count: \(token)")
            }
            count = parsed
        } else {
            count = 1
        }
        return (start, count)
    }
}
