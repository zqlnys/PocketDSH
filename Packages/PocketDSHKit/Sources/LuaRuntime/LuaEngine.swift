import Dispatch
import Foundation
import CLua

public struct LuaExecutionResult: Sendable, Equatable {
    public let stdout: String
    public let stderr: String
    public let exitCode: Int32
    public let duration: TimeInterval

    public var succeeded: Bool { exitCode == 0 }

    public init(
        stdout: String,
        stderr: String,
        exitCode: Int32,
        duration: TimeInterval
    ) {
        self.stdout = stdout
        self.stderr = stderr
        self.exitCode = exitCode
        self.duration = duration
    }
}

public enum LuaRuntimeError: Error, LocalizedError {
    case couldNotCreateContext

    public var errorDescription: String? {
        switch self {
        case .couldNotCreateContext:
            return "Could not create the embedded Lua runtime."
        }
    }
}

private final class LuaOutputBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var stdout = Data()
    private var stderr = Data()

    func append(channel: Int32, bytes: UnsafePointer<CChar>?, length: Int) {
        guard let bytes, length > 0 else { return }
        let data = Data(bytes: bytes, count: length)

        lock.lock()
        defer { lock.unlock() }

        if channel == 0 {
            stdout.append(data)
        } else {
            stderr.append(data)
        }
    }

    func snapshot() -> (stdout: String, stderr: String) {
        lock.lock()
        defer { lock.unlock() }
        return (
            String(decoding: stdout, as: UTF8.self),
            String(decoding: stderr, as: UTF8.self)
        )
    }
}

private let luaWriteCallback: @convention(c) (
    UnsafeMutableRawPointer?,
    Int32,
    UnsafePointer<CChar>?,
    Int
) -> Void = { userdata, channel, bytes, length in
    guard let userdata else { return }
    let buffer = Unmanaged<LuaOutputBuffer>
        .fromOpaque(userdata)
        .takeUnretainedValue()
    buffer.append(channel: channel, bytes: bytes, length: length)
}

private final class LuaRunHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var runtime: OpaquePointer?
    private var isCancelled = false

    func attach(_ runtime: OpaquePointer?) {
        lock.lock()
        self.runtime = runtime
        let shouldCancel = isCancelled
        lock.unlock()

        if shouldCancel, let runtime {
            pocket_lua_cancel(runtime)
        }
    }

    func cancel() {
        lock.lock()
        isCancelled = true
        let runtime = runtime
        lock.unlock()

        if let runtime {
            pocket_lua_cancel(runtime)
        }
    }

    func detach() {
        lock.lock()
        runtime = nil
        lock.unlock()
    }
}

/// A small, sandboxed Lua 5.4 runtime embedded in the app process.
///
/// Supported: base language, `string`, `table`, `math`, `utf8`, `coroutine`,
/// and `print`. Not exposed: `io`, `os`, `package`, `debug`, `dofile`, and
/// `loadfile`. Scripts run on a background serial queue and are checked for a
/// deadline/cancellation every 1,000 VM instructions.
public final class LuaEngine: @unchecked Sendable {
    public init() {}

    public func run(
        _ source: String,
        timeout: TimeInterval = 5
    ) async -> LuaExecutionResult {
        let handle = LuaRunHandle()

        return await withTaskCancellationHandler(operation: {
            await withCheckedContinuation { continuation in
                DispatchQueue.global(qos: .userInitiated).async {
                    continuation.resume(
                        returning: Self.execute(
                            source: source,
                            timeout: timeout,
                            handle: handle
                        )
                    )
                }
            }
        }, onCancel: {
            handle.cancel()
        })
    }

    private static func execute(
        source: String,
        timeout: TimeInterval,
        handle: LuaRunHandle
    ) -> LuaExecutionResult {
        let startedAt = Date()
        let buffer = LuaOutputBuffer()
        let retainedBuffer = Unmanaged.passRetained(buffer)
        let userdata = retainedBuffer.toOpaque()

        guard let runtime = pocket_lua_new(luaWriteCallback, userdata) else {
            retainedBuffer.release()
            return LuaExecutionResult(
                stdout: "",
                stderr: LuaRuntimeError.couldNotCreateContext.localizedDescription,
                exitCode: 1,
                duration: Date().timeIntervalSince(startedAt)
            )
        }

        handle.attach(runtime)
        defer {
            handle.detach()
            pocket_lua_free(runtime)
            retainedBuffer.release()
        }

        var utf8 = source.utf8CString
        let status = utf8.withUnsafeBufferPointer { pointer -> Int32 in
            guard let baseAddress = pointer.baseAddress else {
                return 1
            }
            return Int32(
                pocket_lua_run(
                    runtime,
                    baseAddress,
                    pointer.count - 1,
                    timeout
                )
            )
        }

        let output = buffer.snapshot()
        return LuaExecutionResult(
            stdout: output.stdout,
            stderr: output.stderr,
            exitCode: status,
            duration: Date().timeIntervalSince(startedAt)
        )
    }
}
