import SwiftUI

/// The grouped surface.
///
/// Flat: a soft fill and a hairline, with no shadow. The shadow was there to give cards weight
/// on a cream canvas; on white, next to a chat transcript, it made every group look like a
/// floating panel and the screen read as a dashboard rather than a conversation.
struct PocketCardModifier: ViewModifier {
    var padding: CGFloat
    var raised: Bool

    private var radius: CGFloat { 16 }

    func body(content: Content) -> some View {
        content
            .padding(padding)
            .background {
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .fill(raised ? PocketTheme.surface : PocketTheme.field)
            }
            .overlay {
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .stroke(PocketTheme.stroke, lineWidth: raised ? 1 : 0)
            }
    }
}

extension View {
    /// Groups related content on the canvas. `raised` lifts the card further,
    /// for the one card on a screen that carries the primary action.
    func pocketCard(padding: CGFloat = PocketSpacing.lg, raised: Bool = false) -> some View {
        modifier(PocketCardModifier(padding: padding, raised: raised))
    }
}

struct PressableButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .opacity(configuration.isPressed ? 0.88 : 1)
            .animation(.snappy(duration: 0.18), value: configuration.isPressed)
    }
}

struct PrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, PocketSpacing.lg)
            .padding(.vertical, PocketSpacing.md)
            .background(
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .fill(PocketTheme.accent)
            )
            .opacity(configuration.isPressed ? 0.82 : 1)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.snappy(duration: 0.18), value: configuration.isPressed)
    }
}

struct SecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(PocketTheme.primaryText)
            .padding(.horizontal, PocketSpacing.lg)
            .padding(.vertical, PocketSpacing.md)
            .background(
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .fill(PocketTheme.elevatedSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .stroke(PocketTheme.stroke, lineWidth: 1)
            )
            .opacity(configuration.isPressed ? 0.82 : 1)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.snappy(duration: 0.18), value: configuration.isPressed)
    }
}

/// Small uppercase label above a group of content.
///
/// Uppercasing and letterspacing are Latin typographic devices: Chinese has no
/// case, and tracking pushes its characters apart into something that reads as
/// broken rather than airy. Both are toned down when the interface is Chinese.
struct SectionLabel: View {
    let title: String

    // Sentence case in both languages. The uppercased letterspaced caption was a deliberate
    // choice and it looked like a section header in a form; a chat app labels groups the way it
    // labels everything else.
    private var letterspacing: CGFloat { 0 }

    private var casing: Text.Case? { nil }

    var body: some View {
        Text(NSLocalizedString(title, comment: ""))
            .font(.footnote.weight(.semibold))
            .tracking(letterspacing)
            .textCase(casing)
            .foregroundStyle(PocketTheme.secondaryText)
    }
}

struct StatusPill: View {
    let text: String
    var tint: Color = PocketTheme.accent

    private var letterspacing: CGFloat {
        PocketTheme.isChineseInterface ? 0 : 0.5
    }

    private var casing: Text.Case? {
        PocketTheme.isChineseInterface ? nil : Text.Case.uppercase
    }

    var body: some View {
        Text(NSLocalizedString(text, comment: ""))
            .font(.caption2.weight(.semibold))
            .tracking(letterspacing)
            .textCase(casing)
            .foregroundStyle(tint)
            .padding(.horizontal, PocketSpacing.sm)
            .padding(.vertical, PocketSpacing.xs)
            .background(
                RoundedRectangle(cornerRadius: PocketRadius.xs, style: .continuous)
                    .fill(tint.opacity(0.12))
            )
    }
}

struct IconTile: View {
    let systemName: String
    var tint: Color = PocketTheme.accent
    var size: CGFloat = 42

    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: size * 0.38, weight: .semibold))
            .foregroundStyle(tint)
            .frame(width: size, height: size)
            .background {
                RoundedRectangle(cornerRadius: size * 0.26, style: .continuous)
                    .fill(tint.opacity(0.10))
            }
    }
}

struct EmptyState: View {
    let icon: String
    let title: String
    let message: String
    var actionTitle: String? = nil
    var action: (() -> Void)? = nil

    var body: some View {
        VStack(spacing: PocketSpacing.md) {
            IconTile(systemName: icon, tint: PocketTheme.accent, size: 58)
            Text(NSLocalizedString(title, comment: ""))
                .font(.headline)
                .foregroundStyle(PocketTheme.primaryText)
            Text(NSLocalizedString(message, comment: ""))
                .font(.subheadline)
                .multilineTextAlignment(.center)
                .foregroundStyle(PocketTheme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)
            if let actionTitle, let action {
                Button(actionTitle, action: action)
                    .buttonStyle(PrimaryButtonStyle())
                    .padding(.top, PocketSpacing.xxs)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, PocketSpacing.xxl)
    }
}

struct Banner: View {
    let icon: String
    let title: String
    let message: String
    var tint: Color = PocketTheme.amber

    var body: some View {
        HStack(alignment: .top, spacing: PocketSpacing.md) {
            Image(systemName: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(tint)
                .padding(.top, 1)
            VStack(alignment: .leading, spacing: PocketSpacing.xxs) {
                Text(NSLocalizedString(title, comment: ""))
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                Text(NSLocalizedString(message, comment: ""))
                    .font(.caption)
                    .foregroundStyle(PocketTheme.secondaryText)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(PocketSpacing.md)
        .background {
            RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                .fill(tint.opacity(0.07))
        }
        .overlay {
            RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                .stroke(tint.opacity(0.18), lineWidth: 1)
        }
    }
}
