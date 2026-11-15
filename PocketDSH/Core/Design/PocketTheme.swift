import Foundation
import SwiftUI
import UIKit

enum PocketTheme {
    static let accent = Color(hex: "#3DDC97")
    static let accentDeep = Color(hex: "#20B77A")
    static let blue = Color(hex: "#5B8CFF")
    static let amber = Color(hex: "#F5B84B")
    static let red = Color(hex: "#FF6B6B")
    static let violet = Color(hex: "#A78BFA")

    static let canvas = dynamic(light: "#F4F7FA", dark: "#0B0F14")
    static let canvasElevated = dynamic(light: "#FFFFFF", dark: "#111820")
    static let surface = dynamic(light: "#FFFFFF", dark: "#161E28")
    static let elevatedSurface = dynamic(light: "#EDF1F5", dark: "#1D2733")
    static let codeSurface = dynamic(light: "#F8FAFC", dark: "#0E141B")
    static let primaryText = dynamic(light: "#111820", dark: "#F2F6F8")
    static let secondaryText = dynamic(light: "#647182", dark: "#8B9AAC")
    static let stroke = dynamic(light: "#DCE3EA", dark: "#293542")

    static let background = LinearGradient(
        colors: [canvas, canvasElevated],
        startPoint: .topLeading,
        endPoint: .bottomTrailing
    )

    static let headerGradient = LinearGradient(
        colors: [Color(hex: "#153B33"), Color(hex: "#101B2B")],
        startPoint: .topLeading,
        endPoint: .bottomTrailing
    )

    static let cardShadow = Color.black.opacity(0.16)

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
