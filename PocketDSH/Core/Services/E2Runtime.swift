import Foundation
// ToolExecutionResult lives in PocketDSHCore, and both branches of the build below return it,
// so the import has to sit outside the conditional.
import PocketDSHCore

#if TIER_D_AVAILABLE

/// Tier D: commands run on E2, the aarch64 interpreter written for this project.
///
/// Unlike Tier C, which emulates x86 through iSH and carries GPLv3, E2 interprets the guest's
/// own instruction set directly and needs no JIT, no fork and no exec - which is why it can
/// live inside an app at all.
///
/// There is no persistent shell here yet: each call starts a fresh guest. For busybox that
/// costs a few milliseconds, so it is not worth faking a session before there is one.
@MainActor
final class E2Runtime: ObservableObject {
    enum SessionState: Equatable {
        case idle
        case starting
        case active
        case failed(String)
    }

    @Published private(set) var state: SessionState = .idle

    /// Set when the rootfs or the interpreter is missing, so Settings can say which.
    private(set) var sessionError = ""

    var isSessionActive: Bool { state == .active }
    var workspaceReadOnly = false

    private var busy = false

    /// The guest root. Bundled into the app; see prepareRootfs.
    private var rootfsURL: URL?

    func run(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        guard !busy else {
            return .failure("Tier D is busy. Wait for the running command to finish.")
        }
        busy = true
        defer { busy = false }

        guard let rootfs = prepareRootfs() else {
            state = .failed(sessionError)
            return .failure(sessionError)
        }

        let busybox = rootfs.appendingPathComponent("bin/busybox").path
        guard FileManager.default.isExecutableFile(atPath: busybox) else {
            sessionError = "Tier D needs \(rootfs.path)/bin/busybox. Reinstall the build."
            state = .failed(sessionError)
            return .failure(sessionError)
        }

        state = .starting
        let outURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("e2-\(UUID().uuidString).out")
        defer { try? FileManager.default.removeItem(at: outURL) }

        // The harness takes the program, then its arguments: rune <busybox> sh -c <command>.
        let argv: [String] = [busybox, "sh", "-c", command]
        var cArgs: [UnsafeMutablePointer<CChar>?] = argv.map { strdup($0) }
        defer { for pointer in cArgs { free(pointer) } }
        cArgs.append(nil)

        let status = cArgs.withUnsafeMutableBufferPointer { buffer -> Int32 in
            e2_bridge_run(rootfs.path, outURL.path, Int32(argv.count), buffer.baseAddress)
        }

        let output = (try? String(contentsOf: outURL, encoding: .utf8)) ?? ""
        if status < 0 {
            sessionError = "Tier D could not start the interpreter."
            state = .failed(sessionError)
            return .failure(sessionError)
        }
        state = .active
        let trimmed = output.trimmingCharacters(in: .newlines)
        return .success(trimmed.isEmpty ? "(no output)" : trimmed)
    }

    func discardSession() async {
        state = .idle
    }

    /// Finds the guest root inside the app bundle, unpacking it on first use if it ships as
    /// an archive rather than a directory tree.
    private func prepareRootfs() -> URL? {
        if let cached = rootfsURL, FileManager.default.fileExists(atPath: cached.path) {
            return cached
        }
        let bundle = Bundle.main
        // A tree copied into the bundle keeps its name; an archive is expanded to Application
        // Support so the app bundle stays read-only.
        if let tree = bundle.url(forResource: "alpine-root", withExtension: nil) {
            rootfsURL = tree
            return tree
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                               in: .userDomainMask).first?
            .appendingPathComponent("alpine-root", isDirectory: true)
        if let support, FileManager.default.fileExists(atPath: support.path) {
            rootfsURL = support
            return support
        }
        sessionError = "Tier D has no guest root. The build did not include alpine-root."
        return nil
    }
}

#else

@MainActor
final class E2Runtime: ObservableObject {
    enum SessionState: Equatable {
        case idle
        case starting
        case active
        case failed(String)
    }

    @Published private(set) var state: SessionState = .idle
    var isSessionActive: Bool { false }
    var sessionError: String { "This build does not include the E2 backend." }
    var workspaceReadOnly = false

    func run(command: String, workspace: URL? = nil) async -> ToolExecutionResult {
        .failure("This build does not include the E2 backend.")
    }

    func discardSession() async {}
}

#endif
