import Foundation

public struct WorkspacePathGuard: Sendable {
    public let root: URL

    public init(root: URL) {
        self.root = root.standardizedFileURL
    }

    public func resolve(
        _ path: String,
        mustExist: Bool = false
    ) throws -> URL {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.contains("\0") {
            throw WorkspaceError.invalidPath(path)
        }
        if trimmed.hasPrefix("/") || trimmed.hasPrefix("~") {
            throw WorkspaceError.outsideWorkspace(path)
        }

        let candidate: URL
        if trimmed.isEmpty || trimmed == "." {
            candidate = root
        } else {
            candidate = root.appendingPathComponent(trimmed).standardizedFileURL
        }

        let rootPath = root.path
        let candidatePath = candidate.path
        let rootPrefix = rootPath.hasSuffix("/") ? rootPath : rootPath + "/"

        guard candidatePath == rootPath || candidatePath.hasPrefix(rootPrefix) else {
            throw WorkspaceError.outsideWorkspace(path)
        }

        let realRoot = root.resolvingSymlinksInPath().path
        let realCandidate = candidate.resolvingSymlinksInPath().path
        let realRootPrefix = realRoot.hasSuffix("/") ? realRoot : realRoot + "/"
        guard realCandidate == realRoot || realCandidate.hasPrefix(realRootPrefix) else {
            throw WorkspaceError.outsideWorkspace(path)
        }

        if mustExist {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: candidate.path, isDirectory: &isDirectory) else {
                throw WorkspaceError.notFound(path)
            }
        }

        return candidate
    }

    public func relativePath(for url: URL) throws -> String {
        let rootPath = root.path
        let urlPath = url.standardizedFileURL.path
        let prefix = rootPath.hasSuffix("/") ? rootPath : rootPath + "/"
        guard urlPath.hasPrefix(prefix) else {
            throw WorkspaceError.outsideWorkspace(url.path)
        }
        return String(urlPath.dropFirst(prefix.count))
    }
}
