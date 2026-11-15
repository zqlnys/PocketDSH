import SwiftUI

struct PocketCardModifier: ViewModifier {
    var padding: CGFloat

    func body(content: Content) -> some View {
        content
            .padding(padding)
            .background {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(PocketTheme.surface)
            }
            .overlay {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(PocketTheme.stroke.opacity(0.7), lineWidth: 1)
            }
            .shadow(color: PocketTheme.cardShadow, radius: 0, x: 0, y: 0)
    }
}

extension View {
    func pocketCard(padding: CGFloat = 18) -> some View {
        modifier(PocketCardModifier(padding: padding))
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
            .padding(.horizontal, 18)
            .padding(.vertical, 12)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(PocketTheme.accent))
            .opacity(configuration.isPressed ? 0.82 : 1)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
    }
}

struct SecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(PocketTheme.primaryText)
            .padding(.horizontal, 18)
            .padding(.vertical, 12)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(PocketTheme.elevatedSurface))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(PocketTheme.stroke, lineWidth: 1))
            .opacity(configuration.isPressed ? 0.82 : 1)
    }
}

struct SectionLabel: View {
    let title: String

    var body: some View {
        Text(NSLocalizedString(title, comment: ""))
            .font(.caption.weight(.bold))
            .tracking(1.1)
            .textCase(.uppercase)
            .foregroundStyle(PocketTheme.secondaryText)
    }
}

struct StatusPill: View {
    let text: String
    var tint: Color = PocketTheme.accent

    var body: some View {
        Text(NSLocalizedString(text, comment: ""))
            .font(.caption2.weight(.bold))
            .textCase(.uppercase)
            .tracking(0.6)
            .foregroundStyle(tint)
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(tint.opacity(0.12)))
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
                RoundedRectangle(cornerRadius: 10, style: .continuous)
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
        VStack(spacing: 14) {
            IconTile(systemName: icon, tint: PocketTheme.accent, size: 58)
            Text(NSLocalizedString(title, comment: ""))
                .font(.headline)
                .foregroundStyle(PocketTheme.primaryText)
            Text(NSLocalizedString(message, comment: ""))
                .font(.subheadline)
                .multilineTextAlignment(.center)
                .foregroundStyle(PocketTheme.secondaryText)
            if let actionTitle, let action {
                Button(actionTitle, action: action)
                    .buttonStyle(PrimaryButtonStyle())
                    .padding(.top, 2)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 30)
    }
}

struct Banner: View {
    let icon: String
    let title: String
    let message: String
    var tint: Color = PocketTheme.amber

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(tint)
                .padding(.top, 1)
            VStack(alignment: .leading, spacing: 3) {
                Text(NSLocalizedString(title, comment: ""))
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                Text(NSLocalizedString(message, comment: ""))
                    .font(.caption)
                    .foregroundStyle(PocketTheme.secondaryText)
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(tint.opacity(0.07))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .stroke(tint.opacity(0.18), lineWidth: 1)
        }
    }
}
