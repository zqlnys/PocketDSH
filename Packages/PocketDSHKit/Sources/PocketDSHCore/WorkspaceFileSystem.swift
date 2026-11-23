import Foundation

public struct WorkspaceFileSystem {
    public let root: URL
    public let metadataRoot: URL

    private let pathGuard: WorkspacePathGuard
    private let internalDirectoryName = ".pocketdsh"
    private let maxReadBytes = 2_000_000
    private let maxWriteBytes = 2_000_000
    private let maxSearchFileBytes = 512_000
    private let maxSearchFiles = 3_000

    public init(root: URL, metadataRoot: URL? = nil) {
        self.root = root.standardizedFileURL
        self.metadataRoot = (metadataRoot ?? root.appendingPathComponent(".pocketdsh")).standardizedFileURL
        self.pathGuard = WorkspacePathGuard(root: root)
    }

    // MARK: - Tree and discovery

    public func tree(
        path: String = ".",
        maxDepth: Int = 4,
        includeHidden: Bool = false
    ) throws -> [FileNode] {
        let base = try pathGuard.resolve(path, mustExist: true)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: base.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            throw WorkspaceError.notADirectory(path)
        }
        return try buildNodes(
            at: base,
            relativePath: path == "." ? "" : normalizedRelative(path),
            depth: max(0, maxDepth),
            includeHidden: includeHidden
        )
    }

    public func listFiles(
        path: String = ".",
        maxDepth: Int = 2,
        includeHidden: Bool = false
    ) throws -> [FileEntry] {
        let nodes = try tree(path: path, maxDepth: maxDepth, includeHidden: includeHidden)
        var output: [FileEntry] = []

        func append(_ node: FileNode) {
            output.append(
                FileEntry(
                    path: node.path,
                    name: node.name,
                    isDirectory: node.isDirectory,
                    size: node.size,
                    modifiedAt: node.modifiedAt
                )
            )
            node.children.forEach(append)
        }

        nodes.forEach(append)
        return output
    }

    // MARK: - Read

    public func read(
        path: String,
        offset: Int = 1,
        limit: Int = 400
    ) throws -> FileReadResult {
        let url = try pathGuard.resolve(path, mustExist: true)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            throw WorkspaceError.notAFile(path)
        }

        let size = try fileSize(url)
        guard size <= maxReadBytes else {
            throw WorkspaceError.fileTooLarge(path)
        }

        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }

        if data.contains(0) {
            throw WorkspaceError.fileIsBinary(path)
        }
        guard let text = String(data: data, encoding: .utf8) else {
            throw WorkspaceError.fileIsBinary(path)
        }

        let allLines = text.components(separatedBy: "\n")
        let start = max(1, offset)
        let requestedLimit = max(1, min(limit, 2_000))
        guard start <= allLines.count else {
            return FileReadResult(
                path: normalizedRelative(path),
                lines: [],
                startLine: start,
                endLine: start - 1,
                totalLines: allLines.count,
                truncated: start > 1,
                byteCount: data.count
            )
        }

        let startIndex = start - 1
        let endIndex = min(allLines.count, startIndex + requestedLimit)
        let selected = Array(allLines[startIndex..<endIndex])

        return FileReadResult(
            path: normalizedRelative(path),
            lines: selected,
            startLine: start,
            endLine: start + selected.count - 1,
            totalLines: allLines.count,
            truncated: start > 1 || endIndex < allLines.count,
            byteCount: data.count
        )
    }

    /// Whether a path names a regular file in the workspace.
    ///
    /// Added for `present`, which had been confirming existence by walking the tree to a fixed
    /// depth - so a file seven levels down was reported as missing while it sat there.
    public func isRegularFile(path: String) -> Bool {
        guard let url = try? pathGuard.resolve(path, mustExist: true) else { return false }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else {
            return false
        }
        return !isDirectory.boolValue
    }

    public func readText(path: String) throws -> String {
        let url = try pathGuard.resolve(path, mustExist: true)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            throw WorkspaceError.notAFile(path)
        }
        let size = try fileSize(url)
        guard size <= maxReadBytes else {
            throw WorkspaceError.fileTooLarge(path)
        }
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }
        guard !data.contains(0), let text = String(data: data, encoding: .utf8) else {
            throw WorkspaceError.fileIsBinary(path)
        }
        return text
    }

    // MARK: - Write

    @discardableResult
    public func write(
        path: String,
        content: String,
        overwrite: Bool,
        changeKind: ChangeRecord.Kind = .write
    ) throws -> WriteResult {
        guard content.utf8.count <= maxWriteBytes else {
            throw WorkspaceError.fileTooLarge(path)
        }

        let url = try pathGuard.resolve(path)
        let relative = normalizedRelative(try pathGuard.relativePath(for: url))
        var isDirectory: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory)

        if exists && isDirectory.boolValue {
            throw WorkspaceError.notAFile(path)
        }
        if exists && !overwrite {
            throw WorkspaceError.fileAlreadyExists(relative)
        }

        let parent = url.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(
                at: parent,
                withIntermediateDirectories: true
            )
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }

        let oldText: String?
        if exists {
            let data = try Data(contentsOf: url)
            oldText = data.contains(0) ? nil : String(data: data, encoding: .utf8)
        } else {
            oldText = nil
        }

        let backupPath = exists ? try backUp(url: url, relativePath: relative) : nil
        do {
            try Data(content.utf8).write(to: url, options: .atomic)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }

        let diff = UnifiedDiff.make(
            old: oldText ?? "",
            new: content,
            path: relative
        )
        let summary = exists
            ? "Updated \(relative) (\(content.utf8.count) bytes)"
            : "Created \(relative) (\(content.utf8.count) bytes)"

        try recordChange(
            ChangeRecord(
                path: relative,
                kind: changeKind,
                backupPath: backupPath,
                diff: diff,
                summary: summary
            )
        )

        return WriteResult(
            path: relative,
            bytesWritten: content.utf8.count,
            created: !exists,
            backupPath: backupPath,
            diff: diff
        )
    }

    @discardableResult
    public func createDirectory(path: String) throws -> String {
        let url = try pathGuard.resolve(path)
        var isDirectory: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory)
        if exists {
            guard isDirectory.boolValue else {
                throw WorkspaceError.notAFile(path)
            }
            return normalizedRelative(try pathGuard.relativePath(for: url))
        }
        do {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }
        return normalizedRelative(try pathGuard.relativePath(for: url))
    }

    @discardableResult
    public func importItem(from sourceURL: URL, to destinationPath: String) throws -> String {
        let destination = try pathGuard.resolve(destinationPath)
        var isDirectory: ObjCBool = false
        guard !FileManager.default.fileExists(atPath: destination.path, isDirectory: &isDirectory) else {
            throw WorkspaceError.fileAlreadyExists(destinationPath)
        }
        do {
            try FileManager.default.createDirectory(
                at: destination.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try FileManager.default.copyItem(at: sourceURL, to: destination)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }
        return normalizedRelative(try pathGuard.relativePath(for: destination))
    }

    public func applyPatch(_ patch: String) throws -> PatchResult {
        let changes = try UnifiedPatch.parse(patch)
        var originals: [String: String] = [:]

        for change in changes {
            guard change.oldPath == change.newPath else {
                throw WorkspaceError.patchParse("Renaming files with apply_patch is not supported.")
            }
            let path = change.oldPath
            let url = try pathGuard.resolve(path, mustExist: true)
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
                throw WorkspaceError.notAFile(path)
            }
            let data = try Data(contentsOf: url)
            guard data.count <= maxReadBytes, !data.contains(0), let text = String(data: data, encoding: .utf8) else {
                throw WorkspaceError.fileIsBinary(path)
            }
            originals[path] = text
        }

        let updated = try UnifiedPatch.apply(changes, to: originals)
        var changedPaths: [String] = []
        var diffs: [String: String] = [:]
        var backups: [String: String] = [:]

        for change in changes {
            let path = change.path
            guard let oldText = originals[change.oldPath],
                  let newText = updated[path],
                  oldText != newText else {
                continue
            }

            let diff = UnifiedDiff.make(old: oldText, new: newText, path: path)
            let result = try write(
                path: path,
                content: newText,
                overwrite: true,
                changeKind: .patch
            )
            changedPaths.append(path)
            diffs[path] = diff
            if let backupPath = result.backupPath {
                backups[path] = backupPath
            }
        }

        return PatchResult(
            changedPaths: changedPaths,
            diffs: diffs,
            backupPaths: backups
        )
    }

    // MARK: - Search

    /// Searches file contents.
    ///
    /// `useRegex` switches from a literal substring match to `NSRegularExpression`.
    /// The literal path stays the default because it cannot fail, while a regular
    /// expression can: an unparseable pattern is reported instead of silently
    /// matching nothing, which is the difference between "no results" and "your
    /// pattern is wrong".
    /// What a search found, and whether it looked at everything.
    ///
    /// Returning the matches alone made an incomplete search indistinguishable from a complete one:
    /// past three thousand files the walk stops, and the caller had no way to know that the file it
    /// wanted was never read.
    public struct SearchOutcome: Sendable {
        public let matches: [SearchMatch]
        /// True when the file or result ceiling stopped the walk before the tree was exhausted.
        public let truncated: Bool
        /// Files actually read, so a caller can say how far it got.
        public let scanned: Int
    }

    public func searchOutcome(
        query: String,
        path: String = ".",
        include: String? = nil,
        maxResults: Int = 100,
        useRegex: Bool = false
    ) throws -> SearchOutcome {
        let (matches, truncated, scanned) = try searchDetailed(
            query: query, path: path, include: include,
            maxResults: maxResults, useRegex: useRegex
        )
        return SearchOutcome(matches: matches, truncated: truncated, scanned: scanned)
    }

    /// The matches alone, for callers that do not need to know whether the walk was complete.
    public func search(
        query: String,
        path: String = ".",
        include: String? = nil,
        maxResults: Int = 100,
        useRegex: Bool = false
    ) throws -> [SearchMatch] {
        try searchDetailed(query: query, path: path, include: include,
                           maxResults: maxResults, useRegex: useRegex).0
    }

    private func searchDetailed(
        query: String,
        path: String = ".",
        include: String? = nil,
        maxResults: Int = 100,
        useRegex: Bool = false
    ) throws -> ([SearchMatch], Bool, Int) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return ([], false, 0) }

        var expression: NSRegularExpression?
        if useRegex {
            do {
                expression = try NSRegularExpression(pattern: trimmed, options: [.caseInsensitive])
            } catch {
                throw WorkspaceError.invalidPattern(
                    "That is not a valid regular expression: \(error.localizedDescription)"
                )
            }
        }

        let base = try pathGuard.resolve(path, mustExist: true)
        let limit = max(1, min(maxResults, 500))
        var matches: [SearchMatch] = []
        var visited = 0

        func visit(_ directory: URL) throws {
            guard visited < maxSearchFiles else { return }
            let contents = try FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey],
                options: [.skipsHiddenFiles]
            )

            for url in contents.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                guard visited < maxSearchFiles, matches.count < limit else { return }
                let values = try url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey])
                if values.isDirectory == true {
                    if url.lastPathComponent == internalDirectoryName || url.lastPathComponent == ".git" {
                        continue
                    }
                    try visit(url)
                } else {
                    visited += 1
                    let relative = normalizedRelative(try pathGuard.relativePath(for: url))
                    if let include, !glob(include, matches: relative) {
                        continue
                    }
                    let size = values.fileSize ?? 0
                    guard size <= maxSearchFileBytes else { continue }
                    guard let data = try? Data(contentsOf: url),
                          !data.contains(0),
                          let text = String(data: data, encoding: .utf8) else {
                        continue
                    }

                    for (index, line) in text.components(separatedBy: "\n").enumerated() {
                        guard matches.count < limit else { break }
                        let found: Bool
                        if let expression {
                            let range = NSRange(line.startIndex..<line.endIndex, in: line)
                            found = expression.firstMatch(in: line, options: [], range: range) != nil
                        } else {
                            found = line.range(
                                of: trimmed,
                                options: [.caseInsensitive, .diacriticInsensitive]
                            ) != nil
                        }
                        if found {
                            let preview = line.trimmingCharacters(in: .whitespaces)
                            matches.append(
                                SearchMatch(
                                    path: relative,
                                    line: index + 1,
                                    preview: String(preview.prefix(240))
                                )
                            )
                        }
                    }
                }
            }
        }

        var isDirectory: ObjCBool = false
        if FileManager.default.fileExists(atPath: base.path, isDirectory: &isDirectory), isDirectory.boolValue {
            try visit(base)
        } else {
            let relative = normalizedRelative(try pathGuard.relativePath(for: base))
            guard let data = try? Data(contentsOf: base),
                  !data.contains(0),
                  let text = String(data: data, encoding: .utf8) else {
                return []
            }
            for (index, line) in text.components(separatedBy: "\n").enumerated() {
                guard matches.count < limit else { break }
                if line.range(of: trimmed, options: [.caseInsensitive, .diacriticInsensitive]) != nil {
                    matches.append(
                        SearchMatch(
                            path: relative,
                            line: index + 1,
                            preview: String(line.trimmingCharacters(in: .whitespaces).prefix(240))
                        )
                    )
                }
            }
        }

        // Truncated when either ceiling was reached before the tree ran out.
        let truncated = visited >= maxSearchFiles || matches.count >= limit
        return (matches, truncated, visited)
    }

    // MARK: - Changes and recovery

    public func changes() -> [ChangeRecord] {
        let url = metadataRoot.appendingPathComponent("changes.json")
        guard let data = try? Data(contentsOf: url),
              let records = try? JSONDecoder().decode([ChangeRecord].self, from: data) else {
            return []
        }
        return records
    }

    public func restore(_ record: ChangeRecord) throws {
        guard let backupPath = record.backupPath else {
            throw WorkspaceError.notFound("No backup for \(record.path)")
        }
        let backupURL = try safeMetadataURL(backupPath)
        guard FileManager.default.fileExists(atPath: backupURL.path) else {
            throw WorkspaceError.notFound(backupPath)
        }
        guard let data = try? Data(contentsOf: backupURL),
              let content = String(data: data, encoding: .utf8) else {
            throw WorkspaceError.fileIsBinary(record.path)
        }

        _ = try write(
            path: record.path,
            content: content,
            overwrite: true,
            changeKind: .restore
        )
    }

    // MARK: - Private helpers

    private func buildNodes(
        at directory: URL,
        relativePath: String,
        depth: Int,
        includeHidden: Bool
    ) throws -> [FileNode] {
        let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey]
        let options: FileManager.DirectoryEnumerationOptions = includeHidden ? [] : [.skipsHiddenFiles]
        let contents = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: keys,
            options: options
        )

        var nodes: [FileNode] = []
        for url in contents {
            let name = url.lastPathComponent
            if name == internalDirectoryName || name == ".git" || name == ".build" || name == "node_modules" {
                continue
            }
            let values = try url.resourceValues(forKeys: Set(keys))
            let isDirectory = values.isDirectory ?? false
            let relative = relativePath.isEmpty ? name : "\(relativePath)/\(name)"
            let children: [FileNode]
            if isDirectory && depth > 1 {
                children = try buildNodes(
                    at: url,
                    relativePath: relative,
                    depth: depth - 1,
                    includeHidden: includeHidden
                )
            } else {
                children = []
            }
            nodes.append(
                FileNode(
                    path: relative,
                    name: name,
                    isDirectory: isDirectory,
                    size: Int64(values.fileSize ?? 0),
                    modifiedAt: values.contentModificationDate ?? .distantPast,
                    children: children
                )
            )
        }

        return nodes.sorted { lhs, rhs in
            if lhs.isDirectory != rhs.isDirectory {
                return lhs.isDirectory
            }
            return lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending
        }
    }

    private func normalizedRelative(_ path: String) -> String {
        var result = path
        while result.hasPrefix("./") {
            result.removeFirst(2)
        }
        return result == "." ? "" : result
    }

    private func fileSize(_ url: URL) throws -> Int {
        do {
            let values = try url.resourceValues(forKeys: [.fileSizeKey])
            return values.fileSize ?? 0
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }
    }

    private func backUp(url: URL, relativePath: String) throws -> String {
        do {
            try FileManager.default.createDirectory(
                at: metadataRoot,
                withIntermediateDirectories: true
            )
            let formatter = ISO8601DateFormatter()
            let stamp = formatter.string(from: .now)
                .replacingOccurrences(of: ":", with: "-")
            let relativeBackup = "backups/\(stamp)-\(UUID().uuidString.prefix(8))/\(relativePath)"
            let destination = metadataRoot.appendingPathComponent(relativeBackup)
            try FileManager.default.createDirectory(
                at: destination.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try FileManager.default.copyItem(at: url, to: destination)
            return relativeBackup
        } catch {
            throw WorkspaceError.fileSystem("Could not create backup: \(error.localizedDescription)")
        }
    }

    private func recordChange(_ record: ChangeRecord) throws {
        var records = changes()
        records.append(record)
        if records.count > 200 {
            records.removeFirst(records.count - 200)
        }
        do {
            try FileManager.default.createDirectory(
                at: metadataRoot,
                withIntermediateDirectories: true
            )
            let data = try JSONEncoder().encode(records)
            try data.write(
                to: metadataRoot.appendingPathComponent("changes.json"),
                options: .atomic
            )
        } catch {
            throw WorkspaceError.fileSystem("Could not record change: \(error.localizedDescription)")
        }
    }

    private func safeMetadataURL(_ relativePath: String) throws -> URL {
        let candidate = metadataRoot.appendingPathComponent(relativePath).standardizedFileURL
        let rootPath = metadataRoot.path
        let prefix = rootPath.hasSuffix("/") ? rootPath : rootPath + "/"
        guard candidate.path.hasPrefix(prefix) else {
            throw WorkspaceError.outsideWorkspace(relativePath)
        }
        return candidate
    }

    private func glob(_ pattern: String, matches path: String) -> Bool {
        var expression = "^" + NSRegularExpression.escapedPattern(for: pattern)
        expression = expression
            .replacingOccurrences(of: "\\*", with: ".*")
            .replacingOccurrences(of: "\\?", with: ".")
        expression += "$"
        let target = pattern.contains("/") ? path : (path as NSString).lastPathComponent
        return target.range(of: expression, options: .regularExpression) != nil
    }
}
