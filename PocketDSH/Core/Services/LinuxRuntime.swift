import Foundation
import PocketDSHCore

/// Experimental Tier C command runner.
///
/// The embedded iSH shell currently stays alive after producing output, so this
/// runner deliberately supports one command per app launch. That limitation is
/// reported to the user instead of being hidden.
@MainActor
final class LinuxRuntime {
    private var busy = false
    private var rootfsURL: URL?
    private var prepared = false

    func run(command: String, workspaceRoot: URL? = nil) async -> ToolExecutionResult {
        guard !busy else {
            return .failure("Tier C shell is busy. One Linux command is supported per app launch in this experimental build.")
        }
        guard let root = prepareRootfs() else {
            return .failure("Tier C rootfs is not available in this build.")
        }

        busy = true
        if let workspaceRoot {
            copyWorkspace(workspaceRoot, into: root)
        }
        let commandToRun = "cd /workspace 2>/dev/null || cd /; \(command)"
        let outputURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("pocketdsh-linux-\(UUID().uuidString).txt")
        let rootPath = root.path
        let outputPath = outputURL.path

        Task.detached(priority: .userInitiated) {
            rootPath.withCString { rootPointer in
                commandToRun.withCString { commandPointer in
                    outputPath.withCString { outputPointer in
                        _ = pocket_ish_run(rootPointer, commandPointer, outputPointer)
                    }
                }
            }
        }

        // iSH writes output before its shell task exits; observe a bounded window
        // instead of blocking the UI on the persistent shell task.
        try? await Task.sleep(nanoseconds: 5_000_000_000)
        let raw = (try? String(contentsOf: outputURL, encoding: .utf8)) ?? ""
        let output = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return .success(
            output.isEmpty
                ? "Linux command started; no output was captured in the observation window. Tier C shell remains experimental."
                : "Linux output (Tier C, experimental):\n\(output)"
        )
    }

    private func copyWorkspace(_ source: URL, into rootfs: URL) {
        let destination = rootfs.appendingPathComponent("data", isDirectory: true)
            .appendingPathComponent("workspace", isDirectory: true)
        let fm = FileManager.default
        try? fm.removeItem(at: destination)
        try? fm.createDirectory(at: destination, withIntermediateDirectories: true)
        guard let enumerator = fm.enumerator(
            at: source,
            includingPropertiesForKeys: [.isDirectoryKey, .isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else { return }
        for case let item as URL in enumerator {
            let relative = item.path.replacingOccurrences(of: source.path + "/", with: "")
            let target = destination.appendingPathComponent(relative)
            let values = try? item.resourceValues(forKeys: [.isDirectoryKey])
            if values?.isDirectory == true {
                try? fm.createDirectory(at: target, withIntermediateDirectories: true)
            } else {
                try? fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? fm.copyItem(at: item, to: target)
            }
        }
    }

    private func prepareRootfs() -> URL? {
        if prepared, let rootfsURL {
            return rootfsURL
        }
        guard let bundled = Bundle.main.url(forResource: "rootfs", withExtension: nil) else {
            return nil
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PocketDSH", isDirectory: true)
            .appendingPathComponent("Linux", isDirectory: true)
        let root = support.appendingPathComponent("rootfs", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: root.path) {
                try FileManager.default.copyItem(at: bundled, to: root)
            }
        } catch {
            return nil
        }
        prepared = true
        rootfsURL = root
        return root
    }
}
