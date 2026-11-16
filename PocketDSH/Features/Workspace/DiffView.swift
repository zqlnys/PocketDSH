import SwiftUI
import PocketDSHCore

struct LineDiffView: View {
    let oldText: String
    let newText: String
    let path: String

    private var diff: DiffResult {
        LineDiff.compute(old: oldText, new: newText)
    }

    var body: some View {
        ScrollView([.vertical, .horizontal]) {
            LazyVStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 10) {
                    Text("--- a/\(path)")
                        .foregroundStyle(PocketTheme.red)
                    Text("+++ b/\(path)")
                        .foregroundStyle(PocketTheme.accent)
                }
                .font(.caption.monospaced())
                .padding(.bottom, PocketSpacing.sm)

                if diff.truncated {
                    Text("Large file: diff preview is limited.")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.amber)
                        .padding(.bottom, PocketSpacing.sm)
                }

                ForEach(diff.lines) { line in
                    DiffLineRow(line: line)
                }
            }
            .padding(PocketSpacing.lg)
        }
        .background(PocketTheme.codeSurface)
        .navigationTitle("Diff")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct RawDiffView: View {
    let diff: String
    let path: String

    private var lines: [String] {
        diff.components(separatedBy: "\n")
    }

    var body: some View {
        ScrollView([.vertical, .horizontal]) {
            LazyVStack(alignment: .leading, spacing: 1) {
                Text(path)
                    .font(.caption.monospaced())
                    .foregroundStyle(PocketTheme.secondaryText)
                    .padding(.bottom, 10)

                ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                    Text(line.isEmpty ? " " : line)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(color(for: line))
                        .textSelection(.enabled)
                }
            }
            .padding(PocketSpacing.lg)
        }
        .background(PocketTheme.codeSurface)
        .navigationTitle("Change")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func color(for line: String) -> Color {
        if line.hasPrefix("@@") { return PocketTheme.blue }
        if line.hasPrefix("+++") || line.hasPrefix("---") { return PocketTheme.secondaryText }
        if line.hasPrefix("+") { return PocketTheme.accent }
        if line.hasPrefix("-") { return PocketTheme.red }
        return PocketTheme.primaryText
    }
}

private struct DiffLineRow: View {
    let line: DiffLine

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Text(line.oldLine.map { String($0) } ?? "")
                .frame(width: 34, alignment: .trailing)
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.65))
            Text(line.newLine.map { String($0) } ?? "")
                .frame(width: 34, alignment: .trailing)
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.65))
            Text(prefix)
                .frame(width: 12, alignment: .center)
                .foregroundStyle(foreground)
            Text(line.text.isEmpty ? " " : line.text)
                .foregroundStyle(foreground)
                .textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .font(.system(.caption, design: .monospaced))
        .padding(.vertical, PocketSpacing.xxs)
        .padding(.horizontal, 6)
        .background(background)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    private var prefix: String {
        switch line.kind {
        case .same: return " "
        case .insert: return "+"
        case .remove: return "-"
        }
    }

    private var foreground: Color {
        switch line.kind {
        case .same: return PocketTheme.primaryText
        case .insert: return PocketTheme.accent
        case .remove: return PocketTheme.red
        }
    }

    private var background: Color {
        switch line.kind {
        case .same: return .clear
        case .insert: return PocketTheme.accent.opacity(0.09)
        case .remove: return PocketTheme.red.opacity(0.09)
        }
    }
}
