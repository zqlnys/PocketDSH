import SwiftUI

/// Renders an assistant reply the way the model wrote it.
///
/// Before this, a reply was drawn as one monospaced block of `event.detail`: bullet lists,
/// bold terms, headings and fenced code all appeared as their source characters, and prose set
/// in a code font reads like a log line rather than an answer.
///
/// The split between block and inline work is deliberate. SwiftUI's own Markdown support
/// handles emphasis, inline code and links well, and handles nothing else - no fenced blocks,
/// no lists, no headings. So blocks are parsed here and each block's inline content is handed
/// to `AttributedString(markdown:)`, which keeps the parts that already work.
struct MarkdownText: View {
    let source: String

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(MarkdownText.blocks(in: source).enumerated()), id: \.offset) { _, block in
                view(for: block)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Blocks

    enum Block {
        case paragraph(String)
        case heading(String, Int)
        case bullets([String])
        case numbered([(String, Int)])
        case quote(String)
        case code(String, String?)   // body, language
        case rule
    }

    @ViewBuilder
    private func view(for block: Block) -> some View {
        switch block {
        case .paragraph(let text):
            inline(text)
                .font(.body)
                .foregroundStyle(PocketTheme.primaryText)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)

        case .heading(let text, let level):
            inline(text)
                .font(.system(size: level <= 1 ? 21 : level == 2 ? 18 : 16,
                              weight: .semibold))
                .foregroundStyle(PocketTheme.primaryText)
                .textSelection(.enabled)
                .padding(.top, level <= 2 ? 6 : 2)

        case .bullets(let items):
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        // A real bullet glyph rather than a dash: a dash reads as a hyphen in
                        // prose and disappears at this size.
                        Text("•")
                            .font(.body)
                            .foregroundStyle(PocketTheme.secondaryText)
                        inline(item)
                            .font(.body)
                            .foregroundStyle(PocketTheme.primaryText)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }

        case .numbered(let items):
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text("\(item.1).")
                            .font(.body.monospacedDigit())
                            .foregroundStyle(PocketTheme.secondaryText)
                        inline(item.0)
                            .font(.body)
                            .foregroundStyle(PocketTheme.primaryText)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }

        case .quote(let text):
            HStack(alignment: .top, spacing: 10) {
                RoundedRectangle(cornerRadius: 1.5)
                    .fill(PocketTheme.accent.opacity(0.5))
                    .frame(width: 3)
                inline(text)
                    .font(.callout)
                    .foregroundStyle(PocketTheme.secondaryText)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }

        case .code(let body, let language):
            CodeBlock(code: body, language: language)

        case .rule:
            Rectangle()
                .fill(PocketTheme.secondaryText.opacity(0.2))
                .frame(height: 1)
                .padding(.vertical, 2)
        }
    }

    /// Inline Markdown, with a fallback so a malformed span renders as its own text instead of
    /// vanishing. `AttributedString(markdown:)` throws on a bad escape, and losing an entire
    /// sentence because of one stray asterisk is worse than showing it literally.
    private func inline(_ text: String) -> Text {
        guard let attributed = try? AttributedString(
            markdown: text,
            options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        ) else {
            return Text(text)
        }
        return Text(attributed)
    }

    // MARK: - Parsing

    /// Splits Markdown into blocks.
    ///
    /// Line-based, which is enough for what models actually emit: fenced code, ATX headings,
    /// bullets, numbered items, block quotes, and rules. Anything unrecognised accumulates into
    /// a paragraph, so an odd construct degrades to plain text rather than disappearing.
    static func blocks(in source: String) -> [Block] {
        var blocks: [Block] = []
        var paragraph: [String] = []
        var bullets: [String] = []
        var numbered: [(String, Int)] = []
        var quote: [String] = []

        func flushParagraph() {
            guard !paragraph.isEmpty else { return }
            // Two trailing spaces mean a hard line break; keep it by joining with a newline and
            // letting SwiftUI honour the whitespace.
            blocks.append(.paragraph(paragraph.joined(separator: "\n")))
            paragraph.removeAll()
        }
        func flushList() {
            if !bullets.isEmpty {
                blocks.append(.bullets(bullets))
                bullets.removeAll()
            }
            if !numbered.isEmpty {
                blocks.append(.numbered(numbered))
                numbered.removeAll()
            }
        }
        func flushQuote() {
            guard !quote.isEmpty else { return }
            blocks.append(.quote(quote.joined(separator: " ")))
            quote.removeAll()
        }
        // Narrow versions: switching between bullet and numbered lists should close only the
        // list that ended, not both.
        func flushBulletsOnly() {
            guard !bullets.isEmpty else { return }
            blocks.append(.bullets(bullets)); bullets.removeAll()
        }
        func flushNumberedOnly() {
            guard !numbered.isEmpty else { return }
            blocks.append(.numbered(numbered)); numbered.removeAll()
        }
        func flushAll() {
            flushParagraph(); flushList(); flushQuote()
        }

        let lines = source.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
        var index = 0
        while index < lines.count {
            let line = lines[index]
            let trimmed = line.trimmingCharacters(in: .whitespaces)

            if trimmed.hasPrefix("```") {
                flushAll()
                let language = String(trimmed.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                var body: [String] = []
                index += 1
                while index < lines.count,
                      !lines[index].trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                    body.append(lines[index])
                    index += 1
                }
                // Skip the closing fence when there is one; an unterminated block runs to the
                // end, which is what the user is looking at while the reply streams in.
                if index < lines.count { index += 1 }
                blocks.append(.code(body.joined(separator: "\n"),
                                    language.isEmpty ? nil : language))
                continue
            }

            if trimmed.isEmpty {
                flushAll()
                index += 1
                continue
            }

            if let heading = headingLevel(trimmed) {
                flushAll()
                let text = String(trimmed.dropFirst(heading + 1)).trimmingCharacters(in: .whitespaces)
                blocks.append(.heading(text, heading))
                index += 1
                continue
            }

            if trimmed == "---" || trimmed == "***" || trimmed == "___" {
                flushAll()
                blocks.append(.rule)
                index += 1
                continue
            }

            if trimmed.hasPrefix("> ") || trimmed == ">" {
                flushQuote()
                quote.append(String(trimmed.dropFirst(trimmed == ">" ? 1 : 2)))
                index += 1
                continue
            }
            flushQuote()

            if trimmed.hasPrefix("- ") || trimmed.hasPrefix("* ") || trimmed.hasPrefix("+ ") {
                flushParagraph(); flushNumberedOnly()
                bullets.append(String(trimmed.dropFirst(2)))
                index += 1
                continue
            }

            if let (number, text) = numberedItem(trimmed) {
                flushParagraph(); flushBulletsOnly()
                numbered.append((text, number))
                index += 1
                continue
            }

            flushList()
            paragraph.append(trimmed)
            index += 1
        }
        flushAll()
        return blocks
    }

    private static func headingLevel(_ line: String) -> Int? {
        var level = 0
        for character in line {
            if character == "#" { level += 1 } else { break }
        }
        guard level >= 1, level <= 6, line.count > level else { return nil }
        let after = line.index(line.startIndex, offsetBy: level)
        return line[after] == " " ? level : nil
    }

    private static func numberedItem(_ line: String) -> (Int, String)? {
        var digits = ""
        var rest = line[...]
        while let first = rest.first, first.isNumber {
            digits.append(first)
            rest = rest.dropFirst()
        }
        guard !digits.isEmpty, let number = Int(digits) else { return nil }
        guard rest.hasPrefix(". ") || rest.hasPrefix(") ") else { return nil }
        return (number, String(rest.dropFirst(2)))
    }
}

/// A fenced code block: monospaced, horizontally scrollable, with the language named above it.
///
/// Scrollable rather than wrapped because wrapped code loses its indentation structure, and a
/// reply that quotes Swift or a shell command is unreadable once the lines reflow.
private struct CodeBlock: View {
    let code: String
    let language: String?
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: PocketSpacing.xs) {
                Text(language?.uppercased() ?? "CODE")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(PocketTheme.secondaryText)
                Spacer(minLength: 0)
                Button {
                    UIPasteboard.general.string = code
                    copied = true
                    Task {
                        try? await Task.sleep(nanoseconds: 1_200_000_000)
                        copied = false
                    }
                } label: {
                    Label(copied ? "Copied" : "Copy",
                          systemImage: copied ? "checkmark" : "doc.on.doc")
                        .font(.system(size: 10, weight: .medium))
                        .labelStyle(.titleAndIcon)
                }
                .buttonStyle(.plain)
                .foregroundStyle(PocketTheme.secondaryText)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)

            Divider().opacity(0.3)

            ScrollView(.horizontal, showsIndicators: false) {
                Text(code)
                    .font(.system(size: 12.5, design: .monospaced))
                    .foregroundStyle(PocketTheme.primaryText)
                    .textSelection(.enabled)
                    .padding(10)
            }
        }
        .background {
            RoundedRectangle(cornerRadius: PocketRadius.sm, style: .continuous)
                .fill(PocketTheme.codeSurface)
        }
        .overlay {
            RoundedRectangle(cornerRadius: PocketRadius.sm, style: .continuous)
                .strokeBorder(PocketTheme.secondaryText.opacity(0.15), lineWidth: 0.5)
        }
    }
}
