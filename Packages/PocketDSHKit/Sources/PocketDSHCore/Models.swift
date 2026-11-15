import Foundation

public struct ProjectRecord: Identifiable, Codable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case managed
        case external
    }

    public var id: UUID
    public var name: String
    public var kind: Kind
    public var bookmarkData: Data?
    public var createdAt: Date
    public var lastOpenedAt: Date

    public init(
        id: UUID = UUID(),
        name: String,
        kind: Kind,
        bookmarkData: Data? = nil,
        createdAt: Date = .now,
        lastOpenedAt: Date = .now
    ) {
        self.id = id
        self.name = name
        self.kind = kind
        self.bookmarkData = bookmarkData
        self.createdAt = createdAt
        self.lastOpenedAt = lastOpenedAt
    }
}

public struct FileEntry: Identifiable, Codable, Hashable, Sendable {
    public var id: String { path }
    public let path: String
    public let name: String
    public let isDirectory: Bool
    public let size: Int64
    public let modifiedAt: Date

    public init(
        path: String,
        name: String,
        isDirectory: Bool,
        size: Int64,
        modifiedAt: Date
    ) {
        self.path = path
        self.name = name
        self.isDirectory = isDirectory
        self.size = size
        self.modifiedAt = modifiedAt
    }
}

public struct FileNode: Identifiable, Codable, Hashable, Sendable {
    public var id: String { path }
    public let path: String
    public let name: String
    public let isDirectory: Bool
    public let size: Int64
    public let modifiedAt: Date
    public var children: [FileNode]

    public init(
        path: String,
        name: String,
        isDirectory: Bool,
        size: Int64,
        modifiedAt: Date,
        children: [FileNode] = []
    ) {
        self.path = path
        self.name = name
        self.isDirectory = isDirectory
        self.size = size
        self.modifiedAt = modifiedAt
        self.children = children
    }
}

public struct FileReadResult: Sendable, Equatable {
    public let path: String
    public let lines: [String]
    public let startLine: Int
    public let endLine: Int
    public let totalLines: Int
    public let truncated: Bool
    public let byteCount: Int

    public init(
        path: String,
        lines: [String],
        startLine: Int,
        endLine: Int,
        totalLines: Int,
        truncated: Bool,
        byteCount: Int
    ) {
        self.path = path
        self.lines = lines
        self.startLine = startLine
        self.endLine = endLine
        self.totalLines = totalLines
        self.truncated = truncated
        self.byteCount = byteCount
    }
}

public struct WriteResult: Sendable, Equatable {
    public let path: String
    public let bytesWritten: Int
    public let created: Bool
    public let backupPath: String?
    public let diff: String

    public init(
        path: String,
        bytesWritten: Int,
        created: Bool,
        backupPath: String?,
        diff: String
    ) {
        self.path = path
        self.bytesWritten = bytesWritten
        self.created = created
        self.backupPath = backupPath
        self.diff = diff
    }
}

public struct SearchMatch: Identifiable, Codable, Hashable, Sendable {
    public var id: String { "\(path):\(line):\(preview)" }
    public let path: String
    public let line: Int
    public let preview: String

    public init(path: String, line: Int, preview: String) {
        self.path = path
        self.line = line
        self.preview = preview
    }
}

public struct PatchResult: Sendable, Equatable {
    public let changedPaths: [String]
    public let diffs: [String: String]
    public let backupPaths: [String: String]

    public init(
        changedPaths: [String],
        diffs: [String: String],
        backupPaths: [String: String]
    ) {
        self.changedPaths = changedPaths
        self.diffs = diffs
        self.backupPaths = backupPaths
    }
}

public struct ChangeRecord: Identifiable, Codable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case write
        case patch
        case editor
        case restore
    }

    public let id: UUID
    public let path: String
    public let timestamp: Date
    public let kind: Kind
    public let backupPath: String?
    public let diff: String
    public let summary: String

    public init(
        id: UUID = UUID(),
        path: String,
        timestamp: Date = .now,
        kind: Kind,
        backupPath: String?,
        diff: String,
        summary: String
    ) {
        self.id = id
        self.path = path
        self.timestamp = timestamp
        self.kind = kind
        self.backupPath = backupPath
        self.diff = diff
        self.summary = summary
    }
}
