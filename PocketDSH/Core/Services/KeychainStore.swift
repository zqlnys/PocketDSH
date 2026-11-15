import Foundation
import Security

enum KeychainError: Error, LocalizedError {
    case unhandled(OSStatus)
    case invalidData

    var errorDescription: String? {
        switch self {
        case .unhandled(let status):
            return "Keychain error \(status)"
        case .invalidData:
            return "Keychain returned invalid data."
        }
    }
}

@MainActor
final class KeychainStore: ObservableObject {
    @Published private(set) var hasKey: Bool

    private let service = "com.example.pocketdsh.deepseek"
    private let account = "api-key"

    init() {
        hasKey = false
        refreshStatus()
    }

    func apiKey() -> String? {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        query[kSecAttrSynchronizable as String] = kCFBooleanFalse

        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    func save(_ value: String) throws {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            try delete()
            return
        }
        guard let data = trimmed.data(using: .utf8) else {
            throw KeychainError.invalidData
        }

        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        let attributes: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        ]

        let status = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            var create = query
            create.merge(attributes) { _, new in new }
            let addStatus = SecItemAdd(create as CFDictionary, nil)
            guard addStatus == errSecSuccess else {
                throw KeychainError.unhandled(addStatus)
            }
        } else if status != errSecSuccess {
            throw KeychainError.unhandled(status)
        }

        hasKey = true
    }

    func delete() throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KeychainError.unhandled(status)
        }
        hasKey = false
    }

    private func refreshStatus() {
        hasKey = apiKey()?.isEmpty == false
    }
}
