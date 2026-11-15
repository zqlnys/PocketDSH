import Foundation
import SwiftUI
import UIKit

enum PocketTheme {
    static let accent = Color(hex: "#2F5D50")
    static let accentDeep = Color(hex: "#23483E")
    static let blue = Color(hex: "#3E6FB0")
    static let amber = Color(hex: "#A8732A")
    static let red = Color(hex: "#A84B44")
    static let violet = Color(hex: "#6E5E96")

    static let canvas = dynamic(light: "#F7F6F2", dark: "#0C0D0D")
    static let canvasElevated = dynamic(light: "#FBFAF7", dark: "#111212")
    static let surface = dynamic(light: "#FFFFFF", dark: "#171818")
    static let elevatedSurface = dynamic(light: "#F0EEE8", dark: "#1D1E1E")
    static let codeSurface = dynamic(light: "#FAF9F6", dark: "#101111")
    static let primaryText = dynamic(light: "#171716", dark: "#F1EFE9")
    static let secondaryText = dynamic(light: "#77746C", dark: "#A5A199")
    static let stroke = dynamic(light: "#E2DED4", dark: "#2B2C2B")

    static let background = LinearGradient(
        colors: [canvas, canvasElevated],
        startPoint: .top,
        endPoint: .bottom
    )

    static let cardShadow = Color.black.opacity(0.035)

    static let mono = Font.system(.body, design: .monospaced)
    static let monoSmall = Font.system(.caption, design: .monospaced)

    private static func dynamic(light: String, dark: String) -> Color {
        Color(uiColor: UIColor { traits in
            traits.userInterfaceStyle == .dark
                ? UIColor(hex: dark)
                : UIColor(hex: light)
        })
    }
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
