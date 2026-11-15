import Foundation
import Security

struct JITCapability: Sendable {
    let allowJIT: Bool
    let dynamicCodesigning: Bool
    let unsignedExecutableMemory: Bool

    var isAvailable: Bool {
        allowJIT || dynamicCodesigning || unsignedExecutableMemory
    }

    var entitlementSummary: String {
        if isAvailable {
            var parts: [String] = []
            if allowJIT { parts.append("allow-jit") }
            if dynamicCodesigning { parts.append("dynamic-codesigning") }
            if unsignedExecutableMemory { parts.append("unsigned-executable-memory") }
            return parts.joined(separator: ", ")
        }
        return "No JIT entitlement"
    }

    static func detect() -> JITCapability {
        guard let task = SecTaskCreateFromSelf(kCFAllocatorDefault) else {
            return JITCapability(
                allowJIT: false,
                dynamicCodesigning: false,
                unsignedExecutableMemory: false
            )
        }

        func flag(_ key: String) -> Bool {
            guard let value = SecTaskCopyValueForEntitlement(
                task,
                key as CFString,
                nil
            ) else {
                return false
            }
            return (value as? Bool) ?? false
        }

        return JITCapability(
            allowJIT: flag("com.apple.security.cs.allow-jit"),
            dynamicCodesigning: flag("com.apple.security.cs.dynamic-codesigning"),
            unsignedExecutableMemory: flag("com.apple.security.cs.allow-unsigned-executable-memory")
        )
    }
}

enum RuntimeTier: String, CaseIterable, Identifiable {
    case automatic
    case tierA
    case tierC

    var id: String { rawValue }

    var title: String {
        switch self {
        case .automatic: NSLocalizedString("Automatic", comment: "")
        case .tierA: NSLocalizedString("Tier A · Native Lua", comment: "")
        case .tierC: NSLocalizedString("Tier C · Linux", comment: "")
        }
    }
}

@MainActor
final class RuntimeCapabilityStore: ObservableObject {
    @Published var preferJIT: Bool {
        didSet { defaults.set(preferJIT, forKey: Keys.preferJIT) }
    }

    let jit: JITCapability

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.jit = JITCapability.detect()
        self.preferJIT = defaults.object(forKey: Keys.preferJIT) as? Bool ?? true
    }

    var automaticTier: RuntimeTier {
        jit.isAvailable ? .tierC : .tierA
    }

    /// Tier C is not implemented in this build. Do not silently pretend that a
    /// JIT entitlement turns the native workspace into a Linux runtime.
    var activeTier: RuntimeTier {
        .tierA
    }

    var statusLine: String {
        if jit.isAvailable {
            return NSLocalizedString("JIT detected · Tier C unavailable · Tier A active", comment: "")
        }
        return NSLocalizedString("No JIT · Tier A active", comment: "")
    }

    var detail: String {
        if jit.isAvailable {
            return NSLocalizedString("This build can see a JIT entitlement, but the embedded Linux/Node Tier C backend is not integrated yet. Work continues in the verified native Tier A runtime.", comment: "")
        }
        return NSLocalizedString("No JIT entitlement was detected. The app automatically uses Tier A: native files, agent tools, and the embedded Lua runtime. Tier C requires a separate Linux backend and remains unavailable.", comment: "")
    }

    private enum Keys {
        static let preferJIT = "pocketdsh.runtime.preferJIT"
    }
}
