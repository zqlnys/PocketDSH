import Combine
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
    /// E2: the aarch64 interpreter written for this project. No JIT, no iSH, no GPL.
    case tierD

    var id: String { rawValue }

    /// Label for the workspace badge. Automatic is shown by what it resolved to, never by the
    /// word itself, so this only ever names a concrete tier.
    var badgeLabel: String {
        switch self {
        case .automatic: ""
        case .tierA: "Tier A"
        case .tierC: "Tier C"
        case .tierD: "Tier D"
        }
    }

    /// Compact label for the segmented backend switch.
    var shortTitle: String {
        switch self {
        case .automatic: "Auto"
        case .tierA: "A"
        case .tierC: "C"
        case .tierD: "D"
        }
    }

    var title: String {
        switch self {
        case .automatic: NSLocalizedString("Automatic", comment: "")
        case .tierA: NSLocalizedString("Tier A · Native Lua", comment: "")
        case .tierC: NSLocalizedString("Tier C · Linux (iSH)", comment: "")
        case .tierD: NSLocalizedString("Tier D · E2 (aarch64)", comment: "")
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

    /// True while a Tier C session is actually alive. Set by `AppState` from
    /// `LinuxRuntime.state`; it is never inferred from the JIT probe, because
    /// Tier C is a userland Linux emulator and needs no JIT at all.
    @Published var linuxAvailable = false

    /// True once an E2 command has actually run in this process. Like linuxAvailable this is
    /// never inferred from a preference or a capability probe.
    @Published var e2Available = false

    /// What is really running right now. A preference alone never counts: the
    /// report flips to Tier C only after a session has started.
    var activeTier: RuntimeTier {
        if preferredTier == .tierD && e2Available { return .tierD }
        if preferredTier == .tierC && linuxAvailable { return .tierC }
        return .tierA
    }

    var wantsTierC: Bool {
        preferredTier == .tierC
    }

    var wantsTierD: Bool {
        preferredTier == .tierD
    }

    /// The user asked for Tier C but no session is running, so Tier A is serving
    /// the request. Surfaced in the UI instead of silently falling back.
    var tierCRequestedButUnavailable: Bool {
        preferredTier == .tierC && !linuxAvailable
    }

    var tierCExperimentalLinked: Bool {
        Bundle.main.url(forResource: "rootfs", withExtension: nil) != nil
    }

    var statusLine: String {
        if activeTier == .tierC {
            return NSLocalizedString("Tier C active · iSH Alpine", comment: "")
        }
        if tierCRequestedButUnavailable {
            return NSLocalizedString("Tier C requested · Tier A active", comment: "")
        }
        return NSLocalizedString("Tier A active", comment: "")
    }

    var detail: String {
        if activeTier == .tierC {
            return NSLocalizedString("Tier C is running: an emulated iSH Alpine shell with this project mounted at /workspace. run_linux executes there. Commands are slow compared with native code but need no JIT.", comment: "")
        }
        if tierCRequestedButUnavailable {
            return NSLocalizedString("Tier C is selected but no Linux session is running yet, so Tier A is serving tools. The session starts on the first run_linux call; if it fails, the error is shown instead of pretending Tier C is active.", comment: "")
        }
        return NSLocalizedString("Tier A is active: native files, agent tools, and the embedded Lua runtime. Tier C (emulated iSH Alpine) is available if you select it.", comment: "")
    }

    private enum Keys {
        static let preferJIT = "pocketdsh.runtime.preferJIT"
        static let preferredTier = "pocketdsh.runtime.preferredTier"
    }
}
