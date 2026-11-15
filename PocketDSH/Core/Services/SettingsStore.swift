import Combine
import Foundation

enum AppLanguage: String, CaseIterable, Identifiable {
    case system
    case chinese
    case english

    var id: String { rawValue }

    var title: String {
        switch self {
        case .system: NSLocalizedString("System", comment: "")
        case .chinese: "简体中文"
        case .english: "English"
        }
    }

    var locale: Locale {
        switch self {
        case .system: Locale.current
        case .chinese: Locale(identifier: "zh-Hans")
        case .english: Locale(identifier: "en")
        }
    }
}

@MainActor
final class SettingsStore: ObservableObject {
    @Published var language: AppLanguage {
        didSet { defaults.set(language.rawValue, forKey: Keys.language) }
    }

    @Published var model: String {
        didSet { defaults.set(model, forKey: Keys.model) }
    }

    @Published var maxSteps: Int {
        didSet { defaults.set(maxSteps, forKey: Keys.maxSteps) }
    }

    @Published var showsToolDetails: Bool {
        didSet { defaults.set(showsToolDetails, forKey: Keys.showsToolDetails) }
    }

    let availableModels = ["deepseek-chat", "deepseek-reasoner"]

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        model = defaults.string(forKey: Keys.model) ?? "deepseek-chat"
        language = defaults.string(forKey: Keys.language)
            .flatMap(AppLanguage.init(rawValue:)) ?? .system
        maxSteps = max(3, min(defaults.integer(forKey: Keys.maxSteps) == 0 ? 10 : defaults.integer(forKey: Keys.maxSteps), 20))
        showsToolDetails = defaults.object(forKey: Keys.showsToolDetails) as? Bool ?? true
    }

    private enum Keys {
        static let model = "pocketdsh.settings.model"
        static let language = "pocketdsh.settings.language"
        static let maxSteps = "pocketdsh.settings.maxSteps"
        static let showsToolDetails = "pocketdsh.settings.showsToolDetails"
    }
}
