import Foundation
import PocketDSHCore

/// One long-lived slice across the emulated shell.
///
/// Unlike `run(command:)`, which starts a whole emulated kernel per command and
/// then has to guess when the output is complete, this keeps `/bin/sh` alive and
/// pipes commands in and out. Command boundaries come from a sentinel echoed by
/// the shell, so the tail of the output is no longer lost to a fixed window.
///
/// All emulated work happens on the C side; every method here blocks and must be
/// called off the main actor.
final class PersistentShellSession: @unchecked Sendable {
    private var handle: OpaquePointer?

    /// Starts the shell for `rootfs`, optionally mounting `workspace` at
    /// `/workspace`. Returns nil and fills `error` when the guest fails to boot.
    /// `workspaceReadOnly` mounts the project through a wrapper that rejects
    /// writes. iSH's realfs records the mount flag but never enforces it, so the
    /// flag alone would be cosmetic and the guest would silently mutate project
    /// files outside WorkspaceFileSystem's backups and path guards.
    init?(rootfs: URL, workspace: URL?, workspaceReadOnly: Bool = false, error: inout String) {
        var buffer = [CChar](repeating: 0, count: 256)
        let readOnlyFlag: Int32 = workspaceReadOnly ? 1 : 0
        let created: OpaquePointer? = rootfs.path.withCString { rootPointer in
            if let workspace {
                return workspace.path.withCString { workspacePointer in
                    pocket_shell_start(rootPointer, workspacePointer, readOnlyFlag, &buffer, buffer.count)
                }
            }
            return pocket_shell_start(rootPointer, nil, readOnlyFlag, &buffer, buffer.count)
        }
        guard let created else {
            error = String(cString: buffer)
            return nil
        }
        handle = created
    }

    var isRunning: Bool {
        guard let handle else { return false }
        return pocket_shell_is_running(handle) != 0
    }

    /// Runs one command and returns its combined output.
    @discardableResult
    func run(_ command: String, timeout: TimeInterval = 20) -> String {
        guard let handle else { return "" }
        var out = [CChar](repeating: 0, count: 64 * 1024)
        var truncated: Int32 = 0
        let milliseconds = Int32(max(1, min(timeout, 120) * 1000))
        let written = command.withCString { pointer in
            pocket_shell_exec(handle, pointer, &out, out.count, milliseconds, &truncated)
        }
        guard written > 0 else {
            if written == POCKET_SHELL_TIMEOUT {
                return "<shell timeout>"
            }
            return written == 0 ? "" : "<shell error \(written)>"
        }
        var text = String(cString: out)
        if truncated != 0 {
            text += "\n<output truncated or command still running>"
        }
        return text
    }

    func stop() {
        guard let handle else { return }
        pocket_shell_stop(handle)
        self.handle = nil
    }

    deinit {
        if let handle {
            pocket_shell_stop(handle)
        }
    }
}
