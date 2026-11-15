import Foundation

public enum WorkspaceError: Error, LocalizedError, Equatable {
    case invalidPath(String)
    case outsideWorkspace(String)
    case notFound(String)
    case notAFile(String)
    case notADirectory(String)
    case fileAlreadyExists(String)
    case fileIsBinary(String)
    case fileTooLarge(String)
    case patchMismatch(String)
    case patchParse(String)
    case fileSystem(String)

    public var errorDescription: String? {
        switch self {
        case .invalidPath(let path):
            return "Invalid path: \(path)"
        case .outsideWorkspace(let path):
            return "Path escapes the current workspace: \(path)"
        case .notFound(let path):
            return "File not found: \(path)"
        case .notAFile(let path):
            return "Not a file: \(path)"
        case .notADirectory(let path):
            return "Not a directory: \(path)"
        case .fileAlreadyExists(let path):
            return "File already exists; pass overwrite=true to replace it: \(path)"
        case .fileIsBinary(let path):
            return "File is binary or not valid UTF-8: \(path)"
        case .fileTooLarge(let path):
            return "File exceeds the local safety limit: \(path)"
        case .patchMismatch(let message):
            return "Patch does not apply: \(message)"
        case .patchParse(let message):
            return "Could not parse patch: \(message)"
        case .fileSystem(let message):
            return "Filesystem error: \(message)"
        }
    }
}
