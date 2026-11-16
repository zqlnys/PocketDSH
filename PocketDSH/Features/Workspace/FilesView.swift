import SwiftUI
import UniformTypeIdentifiers
import PocketDSHCore

struct FilesView: View {
    @EnvironmentObject private var appState: AppState
    @ObservedObject var session: WorkspaceSession

    @State private var tree: [FileNode] = []
    @State private var searchQuery = ""
    @State private var searchResults: [SearchMatch] = []
    @State private var errorMessage: String?
    @State private var showNewFile = false
    @State private var showNewFolder = false
    @State private var newPath = ""
    @State private var showImporter = false
    @State private var isImporting = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    projectHeader
                        .listRowInsets(EdgeInsets(top: 6, leading: 20, bottom: 8, trailing: 20))
                        .listRowBackground(Color.clear)
                }

                if !searchResults.isEmpty {
                    Section("Search results") {
                        ForEach(searchResults) { match in
                            NavigationLink(
                                value: FileNode(
                                    path: match.path,
                                    name: (match.path as NSString).lastPathComponent,
                                    isDirectory: false,
                                    size: 0,
                                    modifiedAt: .distantPast
                                )
                            ) {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(match.path)
                                        .font(.caption.monospaced())
                                        .foregroundStyle(PocketTheme.accent)
                                    Text("\(match.line): \(match.preview)")
                                        .font(.caption)
                                        .foregroundStyle(PocketTheme.secondaryText)
                                        .lineLimit(2)
                                }
                                .padding(.vertical, 3)
                            }
                        }
                    }
                }

                Section {
                    if tree.isEmpty {
                        ContentUnavailableView(
                            "Empty workspace",
                            systemImage: "folder",
                            description: Text("Create a file or import a folder to begin.")
                        )
                        .listRowBackground(Color.clear)
                    } else {
                        FileTreeRows(nodes: tree)
                    }
                } header: {
                    HStack {
                        Text("Workspace")
                        Spacer()
                        Text("\(fileCount) files")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(PocketTheme.secondaryText)
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .background(PocketTheme.background)
            .navigationTitle(session.project.name)
            .navigationBarTitleDisplayMode(.inline)
            .searchable(
                text: $searchQuery,
                placement: .navigationBarDrawer(displayMode: .always),
                prompt: "Search files"
            )
            .onSubmit(of: .search) {
                runSearch()
            }
            .onChange(of: searchQuery) { _, value in
                if value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    searchResults = []
                }
            }
            .navigationDestination(for: FileNode.self) { node in
                EditorView(session: session, path: node.path)
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        appState.closeWorkspace()
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .accessibilityLabel("Close project")
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            newPath = "untitled.txt"
                            showNewFile = true
                        } label: {
                            Label("New file", systemImage: "doc.badge.plus")
                        }
                        Button {
                            newPath = "Sources"
                            showNewFolder = true
                        } label: {
                            Label("New folder", systemImage: "folder.badge.plus")
                        }
                        Button {
                            showImporter = true
                        } label: {
                            Label("Import into workspace", systemImage: "square.and.arrow.down")
                        }
                        Divider()
                        ShareLink(item: session.rootURL) {
                            Label("Share workspace folder", systemImage: "square.and.arrow.up")
                        }
                        Button {
                            reload()
                        } label: {
                            Label("Refresh", systemImage: "arrow.clockwise")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("Workspace actions")
                }
            }
            .alert("New file", isPresented: $showNewFile) {
                TextField("Relative path", text: $newPath)
                Button("Create") { createFile() }
                Button("Cancel", role: .cancel) { newPath = "" }
            } message: {
                Text("The file is created inside \(session.project.name).")
            }
            .alert("New folder", isPresented: $showNewFolder) {
                TextField("Relative path", text: $newPath)
                Button("Create") { createFolder() }
                Button("Cancel", role: .cancel) { newPath = "" }
            }
            .fileImporter(
                isPresented: $showImporter,
                allowedContentTypes: [.item]
            ) { result in
                importItem(result)
            }
            .alert(
                "Files",
                isPresented: Binding(
                    get: { errorMessage != nil },
                    set: { if !$0 { errorMessage = nil } }
                )
            ) {
                Button("OK", role: .cancel) { errorMessage = nil }
            } message: {
                Text(errorMessage ?? "")
            }
            .task {
                reload()
            }
            .refreshable {
                reload()
            }
        }
    }

    private var projectHeader: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                IconTile(systemName: "point.3.connected.trianglepath.dotted", tint: PocketTheme.accent, size: 42)
                VStack(alignment: .leading, spacing: 2) {
                    Text(session.project.kind == .managed ? "On-device workspace" : "Authorized external folder")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(PocketTheme.primaryText)
                    Text(session.rootURL.path)
                        .font(.caption2.monospaced())
                        .foregroundStyle(PocketTheme.secondaryText)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
            }

            HStack(spacing: 8) {
                StatusPill(text: "Files", tint: PocketTheme.accent)
                StatusPill(text: "Lua", tint: PocketTheme.blue)
                if isImporting {
                    ProgressView()
                        .controlSize(.mini)
                }
            }
        }
        .padding(PocketSpacing.lg)
        .background {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .fill(PocketTheme.surface)
        }
        .overlay {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .stroke(PocketTheme.stroke.opacity(0.7), lineWidth: 1)
        }
    }

    private var fileCount: Int {
        func count(_ nodes: [FileNode]) -> Int {
            nodes.reduce(0) { $0 + ($1.isDirectory ? count($1.children) : 1) }
        }
        return count(tree)
    }

    private func reload() {
        do {
            tree = try session.fileSystem.tree(maxDepth: 8)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func runSearch() {
        guard !searchQuery.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            searchResults = []
            return
        }
        do {
            searchResults = try session.fileSystem.search(
                query: searchQuery,
                maxResults: 100
            )
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func createFile() {
        let path = newPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !path.isEmpty else { return }
        do {
            _ = try session.fileSystem.write(path: path, content: "", overwrite: false)
            newPath = ""
            reload()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func createFolder() {
        let path = newPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !path.isEmpty else { return }
        do {
            _ = try session.fileSystem.createDirectory(path: path)
            newPath = ""
            reload()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func importItem(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            isImporting = true
            let didAccess = url.startAccessingSecurityScopedResource()
            defer {
                if didAccess { url.stopAccessingSecurityScopedResource() }
                isImporting = false
            }
            do {
                _ = try session.fileSystem.importItem(
                    from: url,
                    to: url.lastPathComponent
                )
                reload()
            } catch {
                errorMessage = error.localizedDescription
            }
        case .failure(let error):
            errorMessage = error.localizedDescription
        }
    }
}

private struct FileTreeRows: View {
    let nodes: [FileNode]

    var body: some View {
        ForEach(nodes) { node in
            if node.isDirectory {
                DisclosureGroup {
                    FileTreeRows(nodes: node.children)
                } label: {
                    row(node)
                }
            } else {
                NavigationLink(value: node) {
                    row(node)
                }
            }
        }
    }

    private func row(_ node: FileNode) -> some View {
        HStack(spacing: 11) {
            Image(systemName: node.isDirectory ? "folder.fill" : icon(for: node.name))
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(node.isDirectory ? PocketTheme.amber : PocketTheme.accent)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 2) {
                Text(node.name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(PocketTheme.primaryText)
                    .lineLimit(1)
                if !node.isDirectory {
                    Text(ByteCountFormatter.string(fromByteCount: node.size, countStyle: .file))
                        .font(.caption2.monospaced())
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }

            Spacer(minLength: 0)
        }
        .padding(.vertical, PocketSpacing.xxs)
    }

    private func icon(for name: String) -> String {
        switch (name as NSString).pathExtension.lowercased() {
        case "swift": return "swift"
        case "js", "ts", "jsx", "tsx": return "curlybraces"
        case "json", "yml", "yaml": return "gearshape.2"
        case "md": return "text.alignleft"
        case "png", "jpg", "jpeg", "webp", "gif": return "photo"
        default: return "doc.text"
        }
    }
}
