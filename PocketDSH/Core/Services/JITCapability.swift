import Foundation

#if canImport(Darwin)
import Darwin
#endif

struct JITCapability: Sendable {
    /// A probe result, not a statement that the app is allowed to run a JIT.
    /// A normal app cannot grant itself JIT; the signature/provisioning profile
    /// must provide the entitlement. This only checks whether this process can
    /// map a small executable page.
    let executableMappingAvailable: Bool
    let probeDetail: String

    var isAvailable: Bool { executableMappingAvailable }

    var entitlementSummary: String {
        executableMappingAvailable ? "Executable mapping available" : probeDetail
    }

    static func detect() -> JITCapability {
        #if targetEnvironment(simulator)
        return JITCapability(
            executableMappingAvailable: false,
            probeDetail: "Simulator does not represent device JIT entitlement"
        )
        #elseif canImport(Darwin)
        let pageSize = sysconf(_SC_PAGESIZE)
        guard pageSize > 0 else {
            return JITCapability(
                executableMappingAvailable: false,
                probeDetail: "Could not read page size"
            )
        }

        let flags = MAP_PRIVATE | MAP_ANON | MAP_JIT
        let pointer = mmap(
            nil,
            pageSize,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            flags,
            -1,
            0
        )

        if pointer == MAP_FAILED {
            let errorNumber = errno
            let message = String(cString: strerror(errorNumber))
            return JITCapability(
                executableMappingAvailable: false,
                probeDetail: "Executable mapping denied: \(message)"
            )
        }

        if let pointer {
            munmap(pointer, pageSize)
        }
        return JITCapability(
            executableMappingAvailable: true,
            probeDetail: "Executable mapping available"
        )
        #else
        return JITCapability(
            executableMappingAvailable: false,
            probeDetail: "Unsupported platform"
        )
        #endif
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

    @Published var preferredTier: RuntimeTier {
        didSet { defaults.set(preferredTier.rawValue, forKey: Keys.preferredTier) }
    }

    let jit: JITCapability

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.jit = JITCapability.detect()
        self.preferJIT = defaults.object(forKey: Keys.preferJIT) as? Bool ?? true
        self.preferredTier = defaults.string(forKey: Keys.preferredTier)
            .flatMap(RuntimeTier.init(rawValue:)) ?? .automatic
    }

    var automaticTier: RuntimeTier {
        jit.isAvailable ? .tierC : .tierA
    }

    /// Tier C is not implemented in this build. Do not silently pretend that a
    /// JIT probe turns the native workspace into a Linux runtime.
    var activeTier: RuntimeTier {
        .tierA
    }

    var wantsTierC: Bool {
        preferredTier == .tierC || (preferredTier == .automatic && jit.isAvailable)
    }

    var tierCExperimentalLinked: Bool {
        Bundle.main.url(forResource: "rootfs", withExtension: nil) != nil
    }

    var statusLine: String {
        if wantsTierC {
            return NSLocalizedString("Tier A default · Tier C experimental", comment: "")
        }
        return NSLocalizedString("Tier A active", comment: "")
    }

    var detail: String {
        if jit.isAvailable {
            return NSLocalizedString("This process can map executable memory, but the embedded Linux/Node Tier C backend is not integrated yet. Work continues in the verified native Tier A runtime.", comment: "")
        }
        return NSLocalizedString("No JIT executable mapping was detected. The app automatically uses Tier A: native files, agent tools, and the embedded Lua runtime. Tier C requires a separate Linux backend and remains unavailable.", comment: "")
    }

    private enum Keys {
        static let preferJIT = "pocketdsh.runtime.preferJIT"
        static let preferredTier = "pocketdsh.runtime.preferredTier"
    }
}
