import Foundation
import PocketDSHCore

/// Whether this process can execute code it loaded at runtime.
///
/// This decides the shape of a future Linux backend:
///
/// - **execution works** → a native userland is viable. Host and guest are both
///   aarch64, so machine code runs directly and only syscalls need forwarding.
///   That is the proot model, and it is roughly an order of magnitude less work
///   than writing an instruction interpreter.
/// - **execution faults** → the backend needs its own aarch64 interpreter, the
///   way iSH does it. Correct, but several months more work.
///
/// The simulator does not represent device policy, so a result from an iPhone is
/// the one that counts.
enum ExecutionProbe {
    struct Report: Equatable {
        let anonymousExecErrno: Int32
        let fileMapErrno: Int32
        let fileProtectErrno: Int32
        let executeStatus: Int32
        let executeValue: Int32
        let anonExecMapErrno: Int32
        let anonExecuteStatus: Int32
        let detail: String

        /// File-backed path: code lives in a file, mapping is promoted to exec.
        /// Refused when the file is not code-signed.
        var fileBackedExecuted: Bool {
            executeStatus == 0 && executeValue == 42
        }

        /// Anonymous path: code is copied into memory the process owns.
        /// This is the loader shape that matters if files must be signed.
        var anonymousExecuted: Bool {
            anonExecuteStatus == 0 && executeValue == 42
        }

        var verdict: String {
            if fileBackedExecuted {
                return "File-backed execution works (returned 42). A native aarch64 userland is viable."
            }
            if anonymousExecuted {
                return "Anonymous-page execution works (returned 42). A native userland is viable with a loader that copies ELF segments into memory."
            }
            if anonExecuteStatus > 0 {
                return "Anonymous page was mapped but running copied code faulted (signal \(anonExecuteStatus)). The backend will need an interpreter."
            }
            if anonExecMapErrno == 0 && anonExecuteStatus == -1 {
                return "Anonymous executable mapping is granted, but running it was not attempted (it previously terminated the process). Treat native execution as unavailable: the backend will need an interpreter."
            }
            if fileProtectErrno != 0 {
                return "Neither path executes runtime code: file mapping was refused (errno \(fileProtectErrno)) and the anonymous page could not run copied code. The backend will need an interpreter."
            }
            return "Could not establish either execution path."
        }

        var environmentNote: String {
            #if targetEnvironment(simulator)
            return "Simulator: does not represent device code-signing policy. Run on an iPhone for the real answer."
            #else
            return "Device run: this reflects real code-signing policy."
            #endif
        }
    }

    /// - Parameter attemptAnonymousExecution: off by default. This step can
    ///   terminate the process without a catchable signal on device, so it only
    ///   runs when the user explicitly asks for it.
    static func run(attemptAnonymousExecution: Bool = false) -> Report {
        var result = pocket_exec_probe_result()
        let status = pocket_exec_probe(&result, attemptAnonymousExecution ? 1 : 0)
        guard status == 0 else {
            return Report(
                anonymousExecErrno: 0,
                fileMapErrno: 0,
                fileProtectErrno: 0,
                executeStatus: -1,
                executeValue: 0,
                anonExecMapErrno: 0,
                anonExecuteStatus: -1,
                detail: "probe failed to run (status \(status))"
            )
        }
        return Report(
            anonymousExecErrno: result.anonymous_exec_errno,
            fileMapErrno: result.file_map_errno,
            fileProtectErrno: result.file_protect_errno,
            executeStatus: result.execute_status,
            executeValue: result.execute_value,
            anonExecMapErrno: result.anon_exec_map_errno,
            anonExecuteStatus: result.anon_execute_status,
            detail: withUnsafeBytes(of: result.detail) { raw in
                String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
            }
        )
    }
}
