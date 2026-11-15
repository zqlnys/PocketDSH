import Foundation

public struct DiffLine: Identifiable, Hashable, Sendable {
    public enum Kind: String, Hashable, Sendable {
        case same
        case insert
        case remove
    }

    public let kind: Kind
    public let text: String
    public let oldLine: Int?
    public let newLine: Int?

    public var id: String {
        "\(kind.rawValue):\(oldLine ?? -1):\(newLine ?? -1):\(text)"
    }

    public init(kind: Kind, text: String, oldLine: Int?, newLine: Int?) {
        self.kind = kind
        self.text = text
        self.oldLine = oldLine
        self.newLine = newLine
    }
}

public struct DiffResult: Sendable, Equatable {
    public let lines: [DiffLine]
    public let truncated: Bool

    public init(lines: [DiffLine], truncated: Bool) {
        self.lines = lines
        self.truncated = truncated
    }
}

public enum LineDiff {
    public static func compute(
        old oldText: String,
        new newText: String,
        maxLines: Int = 1200
    ) -> DiffResult {
        let oldLines = oldText.components(separatedBy: "\n")
        let newLines = newText.components(separatedBy: "\n")

        if oldLines.count > maxLines || newLines.count > maxLines {
            return DiffResult(
                lines: [
                    DiffLine(
                        kind: .same,
                        text: "Diff omitted: file has more than \(maxLines) lines.",
                        oldLine: nil,
                        newLine: nil
                    )
                ],
                truncated: true
            )
        }

        let rows = oldLines.count
        let columns = newLines.count
        var lcs = Array(
            repeating: Array(repeating: Int32(0), count: columns + 1),
            count: rows + 1
        )

        if rows > 0 && columns > 0 {
            for row in stride(from: rows - 1, through: 0, by: -1) {
                for column in stride(from: columns - 1, through: 0, by: -1) {
                    if oldLines[row] == newLines[column] {
                        lcs[row][column] = lcs[row + 1][column + 1] + 1
                    } else {
                        lcs[row][column] = max(lcs[row + 1][column], lcs[row][column + 1])
                    }
                }
            }
        }

        var result: [DiffLine] = []
        var oldIndex = 0
        var newIndex = 0
        var oldLineNumber = 1
        var newLineNumber = 1

        while oldIndex < rows && newIndex < columns {
            if oldLines[oldIndex] == newLines[newIndex] {
                result.append(
                    DiffLine(
                        kind: .same,
                        text: oldLines[oldIndex],
                        oldLine: oldLineNumber,
                        newLine: newLineNumber
                    )
                )
                oldIndex += 1
                newIndex += 1
                oldLineNumber += 1
                newLineNumber += 1
            } else if lcs[oldIndex + 1][newIndex] >= lcs[oldIndex][newIndex + 1] {
                result.append(
                    DiffLine(
                        kind: .remove,
                        text: oldLines[oldIndex],
                        oldLine: oldLineNumber,
                        newLine: nil
                    )
                )
                oldIndex += 1
                oldLineNumber += 1
            } else {
                result.append(
                    DiffLine(
                        kind: .insert,
                        text: newLines[newIndex],
                        oldLine: nil,
                        newLine: newLineNumber
                    )
                )
                newIndex += 1
                newLineNumber += 1
            }
        }

        while oldIndex < rows {
            result.append(
                DiffLine(
                    kind: .remove,
                    text: oldLines[oldIndex],
                    oldLine: oldLineNumber,
                    newLine: nil
                )
            )
            oldIndex += 1
            oldLineNumber += 1
        }

        while newIndex < columns {
            result.append(
                DiffLine(
                    kind: .insert,
                    text: newLines[newIndex],
                    oldLine: nil,
                    newLine: newLineNumber
                )
            )
            newIndex += 1
            newLineNumber += 1
        }

        return DiffResult(lines: result, truncated: false)
    }
}

public enum UnifiedDiff {
    /// Produces a complete-file unified diff. It is intentionally not a
    /// minimal-hunk diff: it is easy to validate and still applies with the
    /// local patch engine.
    public static func make(old oldText: String, new newText: String, path: String) -> String {
        let diff = LineDiff.compute(old: oldText, new: newText)
        let oldCount = oldText.components(separatedBy: "\n").count
        let newCount = newText.components(separatedBy: "\n").count
        let oldStart = oldCount > 0 ? 1 : 0
        let newStart = newCount > 0 ? 1 : 0

        var lines = [
            "--- a/\(path)",
            "+++ b/\(path)",
            "@@ -\(oldStart),\(oldCount) +\(newStart),\(newCount) @@"
        ]

        for line in diff.lines {
            switch line.kind {
            case .same:
                lines.append(" " + line.text)
            case .remove:
                lines.append("-" + line.text)
            case .insert:
                lines.append("+" + line.text)
            }
        }

        if diff.truncated {
            lines.append("\\ Diff truncated")
        }
        return lines.joined(separator: "\n")
    }
}
