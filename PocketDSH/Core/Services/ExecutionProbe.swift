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
        let detail: String

        var executedSuccessfully: Bool {
            executeStatus == 0 && executeValue == 42
        }

        /// Mapping and permission promotion both worked, so only the execution
        /// step decided the outcome.
        var codeWasMappable: Bool {
            fileMapErrno == 0 && fileProtectErrno == 0
        }

        var verdict: String {
            if executedSuccessfully {
                return "Executed loaded code (returned 42). A native aarch64 userland is viable."
            }
            if codeWasMappable {
                return "Code mapped and was marked executable, but running it faulted (signal \(executeStatus)). The backend will need an interpreter."
            }
            if fileMapErrno != 0 {
                return "Could not map the code file (errno \(fileMapErrno))."
            }
            return "mprotect(PROT_EXEC) was refused (errno \(fileProtectErrno)). The backend will need an interpreter."
        }

        var environmentNote: String {
            #if targetEnvironment(simulator)
            return "Simulator: does not represent device code-signing policy. Run on an iPhone for the real answer."
            #else
            return "Device run: this reflects real code-signing policy."
            #endif
        }
    }

    static func run() -> Report {
        var result = pocket_exec_probe_result()
        let status = pocket_exec_probe(&result)
        guard status == 0 else {
            return Report(
                anonymousExecErrno: 0,
                fileMapErrno: 0,
                fileProtectErrno: 0,
                executeStatus: -1,
                executeValue: 0,
                detail: "probe failed to run (status \(status))"
            )
        }
        return Report(
            anonymousExecErrno: result.anonymous_exec_errno,
            fileMapErrno: result.file_map_errno,
            fileProtectErrno: result.file_protect_errno,
            executeStatus: result.execute_status,
            executeValue: result.execute_value,
            detail: withUnsafeBytes(of: result.detail) { raw in
                String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
            }
        )
    }
}
