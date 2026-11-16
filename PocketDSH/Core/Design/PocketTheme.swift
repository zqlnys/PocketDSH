import Foundation
import SwiftUI
import UIKit

/// Design tokens.
///
/// Views should not invent their own spacing, corner radii or shadows. Before
/// this file carried scales, `ProjectsView` alone used paddings of 2, 4, 8, 10,
/// 12, 14, 16, 20, 22, 24, 30, 32 and 34, and five different corner radii were
/// in play across the components - which reads as visually restless even when
/// no single screen looks wrong.
enum PocketTheme {
    // MARK: Brand

    static let accent = Color(hex: "#2F5D50")
    static let accentDeep = Color(hex: "#23483E")
    static let blue = Color(hex: "#3E6FB0")
    static let amber = Color(hex: "#A8732A")
    static let red = Color(hex: "#A84B44")
    static let violet = Color(hex: "#6E5E96")

    // MARK: Surfaces

    static let canvas = dynamic(light: "#F7F6F2", dark: "#0C0D0D")
    static let canvasElevated = dynamic(light: "#FBFAF7", dark: "#111212")
    static let surface = dynamic(light: "#FFFFFF", dark: "#171818")
    static let elevatedSurface = dynamic(light: "#F0EEE8", dark: "#1D1E1E")
    static let codeSurface = dynamic(light: "#FAF9F6", dark: "#101111")
    static let primaryText = dynamic(light: "#171716", dark: "#F1EFE9")
    static let secondaryText = dynamic(light: "#77746C", dark: "#A5A199")
    static let stroke = dynamic(light: "#E2DED4", dark: "#2B2C2B")

    /// Slightly stronger than `stroke`, for the edge of a card that has to read
    /// against a tinted canvas.
    static let cardStroke = dynamic(light: "#E7E3DA", dark: "#262727")

    static let background = LinearGradient(
        colors: [canvas, canvasElevated],
        startPoint: .top,
        endPoint: .bottom
    )

    // MARK: Depth
    //
    // The previous card shadow had `radius: 0`, which draws nothing, so every
    // card was separated from the canvas by a single hairline stroke and the
    // interface read as flat paper. A real, restrained shadow is what gives the
    // lists and cards their weight.

    static let cardShadowColor = Color.black.opacity(0.06)
    static let cardShadowRadius: CGFloat = 14
    static let cardShadowY: CGFloat = 4

    static let raisedShadowColor = Color.black.opacity(0.10)
    static let raisedShadowRadius: CGFloat = 22
    static let raisedShadowY: CGFloat = 8

    // MARK: Typography

    static let mono = Font.system(.body, design: .monospaced)
    static let monoSmall = Font.system(.caption, design: .monospaced)

    /// The display face is a serif, which flatters Latin headings but sets
    /// Chinese heavily. `displayFont(size:)` falls back to the system face when
    /// the interface language is Chinese.
    static func displayFont(size: CGFloat) -> Font {
        if PocketTheme.isChineseInterface {
            return .system(size: size, weight: .semibold)
        }
        return .system(size: size, weight: .medium, design: .serif)
    }

    /// Section headers are uppercased and letterspaced, which is a Latin idiom.
    /// Chinese characters have no case and gain nothing from tracking, so the
    /// label component consults this to decide how much styling to apply.
    static var isChineseInterface: Bool {
        let code = Locale.current.language.languageCode?.identifier ?? "en"
        return code == "zh"
    }

    private static func dynamic(light: String, dark: String) -> Color {
        Color(uiColor: UIColor { traits in
            traits.userInterfaceStyle == .dark
                ? UIColor(hex: dark)
                : UIColor(hex: light)
        })
    }
}

/// Spacing scale. Use these instead of literals so vertical rhythm stays
/// consistent between screens.
enum PocketSpacing {
    static let xxs: CGFloat = 2
    static let xs: CGFloat = 4
    static let sm: CGFloat = 8
    static let md: CGFloat = 12
    static let lg: CGFloat = 16
    static let xl: CGFloat = 24
    static let xxl: CGFloat = 32
    static let xxxl: CGFloat = 44
}

/// Corner radii. Before this existed the app mixed 6, 8, 10, 12 and 16.
enum PocketRadius {
    static let xs: CGFloat = 6
    static let sm: CGFloat = 10
    static let md: CGFloat = 12
    static let lg: CGFloat = 16
    static let xl: CGFloat = 22
}

extension Color {
    init(hex: String) {
        self.init(uiColor: UIColor(hex: hex))
    }
}

extension UIColor {
    convenience init(hex: String) {
        var value: UInt64 = 0
        let cleaned = hex.trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
        Scanner(string: cleaned).scanHexInt64(&value)

        let red: CGFloat
        let green: CGFloat
        let blue: CGFloat
        let alpha: CGFloat

        switch cleaned.count {
        case 8:
            red = CGFloat((value >> 24) & 0xFF) / 255
            green = CGFloat((value >> 16) & 0xFF) / 255
            blue = CGFloat((value >> 8) & 0xFF) / 255
            alpha = CGFloat(value & 0xFF) / 255
        default:
            red = CGFloat((value >> 16) & 0xFF) / 255
            green = CGFloat((value >> 8) & 0xFF) / 255
            blue = CGFloat(value & 0xFF) / 255
            alpha = 1
        }

        self.init(red: red, green: green, blue: blue, alpha: alpha)
    }
}
