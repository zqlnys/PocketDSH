import Foundation
import UniformTypeIdentifiers
import PocketDSHCore

struct WorkspaceLocation {
    let root: URL
    let isSecurityScoped: Bool
}

@MainActor
final class ProjectStore: ObservableObject {
    @Published private(set) var projects: [ProjectRecord] = []

    private let fileManager = FileManager.default
    private let appSupportRoot: URL
    private let projectsRoot: URL
    private let metadataRoot: URL
    private let indexURL: URL

    init() {
        let support = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? fileManager.temporaryDirectory
        let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? fileManager.temporaryDirectory

        appSupportRoot = support.appendingPathComponent("PocketDSH", isDirectory: true)
        projectsRoot = documents
            .appendingPathComponent("PocketDSHProjects", isDirectory: true)
        metadataRoot = appSupportRoot.appendingPathComponent("Projects", isDirectory: true)
        indexURL = appSupportRoot.appendingPathComponent("projects.json")

        createDirectories()
        load()
    }

    func createProject(named rawName: String) throws -> ProjectRecord {
        let name = sanitizedName(rawName)
        let record = ProjectRecord(name: name, kind: .managed)
        let workspace = projectsRoot
            .appendingPathComponent(record.id.uuidString, isDirectory: true)
            .appendingPathComponent("workspace", isDirectory: true)
        do {
            try fileManager.createDirectory(at: workspace, withIntermediateDirectories: true)
            try fileManager.createDirectory(
                at: metadataDirectory(for: record.id),
                withIntermediateDirectories: true
            )
            try fileManager.createDirectory(at: projectsRoot, withIntermediateDirectories: true)
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }
        projects.insert(record, at: 0)
        save()
        return record
    }

    func importManagedProject(from sourceURL: URL) throws -> ProjectRecord {
        let didAccess = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if didAccess { sourceURL.stopAccessingSecurityScopedResource() }
        }

        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            throw WorkspaceError.notADirectory(sourceURL.lastPathComponent)
        }

        let record = ProjectRecord(
            name: sanitizedName(sourceURL.deletingPathExtension().lastPathComponent),
            kind: .managed
        )
        let parent = projectsRoot.appendingPathComponent(record.id.uuidString, isDirectory: true)
        let workspace = parent.appendingPathComponent("workspace", isDirectory: true)
        do {
            try fileManager.createDirectory(at: parent, withIntermediateDirectories: true)
            try fileManager.copyItem(at: sourceURL, to: workspace)
            try fileManager.createDirectory(
                at: metadataDirectory(for: record.id),
                withIntermediateDirectories: true
            )
        } catch {
            throw WorkspaceError.fileSystem(error.localizedDescription)
        }

        projects.insert(record, at: 0)
        save()
        return record
    }

    func addExternalProject(from sourceURL: URL) throws -> ProjectRecord {
        let didAccess = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if didAccess { sourceURL.stopAccessingSecurityScopedResource() }
        }

        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            throw WorkspaceError.notADirectory(sourceURL.lastPathComponent)
        }

        let bookmark: Data
        do {
            bookmark = try sourceURL.bookmarkData(
                options: [.minimalBookmark],
                includingResourceValuesForKeys: nil,
                relativeTo: nil
            )
        } catch {
            throw WorkspaceError.fileSystem("Could not remember this folder: \(error.localizedDescription)")
        }

        let record = ProjectRecord(
            name: sanitizedName(sourceURL.lastPathComponent),
            kind: .external,
            bookmarkData: bookmark
        )
        try fileManager.createDirectory(
            at: metadataDirectory(for: record.id),
            withIntermediateDirectories: true
        )
        projects.insert(record, at: 0)
        save()
        return record
    }

    func location(for record: ProjectRecord) throws -> WorkspaceLocation {
        switch record.kind {
        case .managed:
            let root = projectsRoot
                .appendingPathComponent(record.id.uuidString, isDirectory: true)
                .appendingPathComponent("workspace", isDirectory: true)
            guard fileManager.fileExists(atPath: root.path) else {
                throw WorkspaceError.notFound(record.name)
            }
            return WorkspaceLocation(root: root, isSecurityScoped: false)
        case .external:
            guard let bookmark = record.bookmarkData else {
                throw WorkspaceError.notFound(record.name)
            }
            var stale = false
            let url = try URL(
                resolvingBookmarkData: bookmark,
                options: [.withoutUI],
                relativeTo: nil,
                bookmarkDataIsStale: &stale
            )
            return WorkspaceLocation(root: url, isSecurityScoped: true)
        }
    }

    func metadataDirectory(for id: UUID) -> URL {
        metadataRoot.appendingPathComponent(id.uuidString, isDirectory: true)
    }

    func touch(_ record: ProjectRecord) {
        guard let index = projects.firstIndex(where: { $0.id == record.id }) else { return }
        projects[index].lastOpenedAt = .now
        let updated = projects[index]
        projects.remove(at: index)
        projects.insert(updated, at: 0)
        save()
    }

    func rename(_ record: ProjectRecord, to rawName: String) {
        guard let index = projects.firstIndex(where: { $0.id == record.id }) else { return }
        projects[index].name = sanitizedName(rawName)
        save()
    }

    func delete(_ record: ProjectRecord) {
        if record.kind == .managed {
            let workspaceParent = projectsRoot.appendingPathComponent(record.id.uuidString, isDirectory: true)
            try? fileManager.removeItem(at: workspaceParent)
        }
        try? fileManager.removeItem(at: metadataDirectory(for: record.id))
        projects.removeAll { $0.id == record.id }
        save()
    }

    private func createDirectories() {
        for url in [appSupportRoot, projectsRoot, metadataRoot] {
            try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        }
    }

    private func load() {
        // A missing index and an unreadable one are different, and the difference matters because
        // the next save writes whatever is in memory: an unreadable index that quietly became an
        // empty list was overwritten by the first project the user opened, and every other project
        // was gone with nothing having reported it.
        guard fileManager.fileExists(atPath: indexURL.path) else {
            projects = []
            return
        }
        guard let data = try? Data(contentsOf: indexURL) else {
            projects = []
            loadError = "The project list could not be read. It has been left on disk and will not "
                + "be overwritten; projects you open now are not saved."
            writesBlocked = true
            return
        }
        guard let records = try? JSONDecoder().decode([ProjectRecord].self, from: data) else {
            projects = []
            loadError = "The project list is unreadable, so it was not loaded and will not be "
                + "overwritten. Projects you open now are not saved."
            writesBlocked = true
            return
        }
        projects = records.sorted { $0.lastOpenedAt > $1.lastOpenedAt }
    }

    private func save() {
        guard !writesBlocked else { return }
        do {
            try fileManager.createDirectory(at: appSupportRoot, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(projects)
            try data.write(to: indexURL, options: .atomic)
        } catch {
            // A failed metadata write must not crash the workspace. The UI can
            // still operate on the files; the next successful save repairs it.
        }
    }

    /// Says why the list is not being written, when it is not.
    private(set) var loadError: String?

    /// Set when an existing index could not be used. Saving stops, because the alternative is
    /// writing over the file that could not be read.
    private var writesBlocked = false

    private func sanitizedName(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return "Untitled Project" }
        return String(trimmed.prefix(80))
    }
}
