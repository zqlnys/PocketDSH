import Combine
import Foundation

@MainActor
final class SettingsStore: ObservableObject {
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
        maxSteps = max(3, min(defaults.integer(forKey: Keys.maxSteps) == 0 ? 10 : defaults.integer(forKey: Keys.maxSteps), 20))
        showsToolDetails = defaults.object(forKey: Keys.showsToolDetails) as? Bool ?? true
    }

    private enum Keys {
        static let model = "pocketdsh.settings.model"
        static let maxSteps = "pocketdsh.settings.maxSteps"
        static let showsToolDetails = "pocketdsh.settings.showsToolDetails"
    }
}
