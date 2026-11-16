import SwiftUI
import UniformTypeIdentifiers
import PocketDSHCore

struct EditorView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var session: WorkspaceSession
    let path: String

    @State private var text = ""
    @State private var original = ""
    @State private var isLoading = true
    @State private var isSaving = false
    @State private var errorMessage: String?
    @State private var showDiff = false
    @State private var showDiscard = false
    @State private var showExporter = false
    @State private var exportDocument: TextFileDocument?
    @State private var savedMessage: String?

    private var isDirty: Bool { text != original }
    private var lineCount: Int { text.components(separatedBy: "\n").count }

    var body: some View {
        ZStack {
            PocketTheme.background
                .ignoresSafeArea()

            if isLoading {
                ProgressView("Loading file…")
                    .tint(PocketTheme.accent)
            } else {
                VStack(spacing: 0) {
                    metaBar
                    TextEditor(text: $text)
                        .font(.system(.body, design: .monospaced))
                        .foregroundStyle(PocketTheme.primaryText)
                        .scrollContentBackground(.hidden)
                        .background(PocketTheme.codeSurface)
                        .padding(PocketSpacing.sm)
                }
            }
        }
        .navigationTitle(path)
        .navigationBarTitleDisplayMode(.inline)
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button {
                    if isDirty {
                        showDiscard = true
                    } else {
                        dismiss()
                    }
                } label: {
                    Image(systemName: "chevron.left")
                }
                .accessibilityLabel("Back")
            }

            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button {
                        save()
                    } label: {
                        Label("Save", systemImage: "square.and.arrow.down")
                    }
                    .disabled(isSaving)

                    Button {
                        showDiff = true
                    } label: {
                        Label("View diff", systemImage: "plus.forwardslash.minus")
                    }
                    .disabled(!isDirty)

                    Button {
                        exportDocument = TextFileDocument(text: text)
                        showExporter = true
                    } label: {
                        Label("Export file", systemImage: "square.and.arrow.up")
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("Editor actions")
            }
        }
        .confirmationDialog(
            "Discard unsaved changes?",
            isPresented: $showDiscard,
            titleVisibility: .visible
        ) {
            Button("Discard changes", role: .destructive) {
                dismiss()
            }
            Button("Keep editing", role: .cancel) {}
        } message: {
            Text("The file has changes that have not been written to disk.")
        }
        .sheet(isPresented: $showDiff) {
            NavigationStack {
                LineDiffView(oldText: original, newText: text, path: path)
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) {
                            Button("Done") { showDiff = false }
                        }
                    }
            }
        }
        .fileExporter(
            isPresented: $showExporter,
            document: exportDocument,
            contentType: .plainText,
            defaultFilename: (path as NSString).lastPathComponent
        ) { result in
            if case .failure(let error) = result {
                errorMessage = error.localizedDescription
            }
        }
        .alert(
            "Editor",
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
            load()
        }
    }

    private var metaBar: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(isDirty ? PocketTheme.amber : PocketTheme.accent)
                .frame(width: 7, height: 7)
            Text(isDirty ? "Unsaved" : "Saved")
                .font(.caption.weight(.semibold))
                .foregroundStyle(isDirty ? PocketTheme.amber : PocketTheme.accent)
            Text("·")
                .foregroundStyle(PocketTheme.secondaryText)
            Text("\(lineCount) lines")
                .font(.caption.monospacedDigit())
                .foregroundStyle(PocketTheme.secondaryText)
            Spacer()
            if let savedMessage {
                Text(savedMessage)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.accent)
            }
            if isSaving {
                ProgressView()
                    .controlSize(.mini)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, PocketSpacing.sm)
        .background(PocketTheme.surface)
    }

    private func load() {
        do {
            let loaded = try session.fileSystem.readText(path: path)
            text = loaded
            original = loaded
            isLoading = false
        } catch {
            isLoading = false
            errorMessage = error.localizedDescription
        }
    }

    private func save() {
        guard !isSaving else { return }
        isSaving = true
        defer { isSaving = false }

        do {
            _ = try session.fileSystem.write(
                path: path,
                content: text,
                overwrite: true,
                changeKind: .editor
            )
            original = text
            savedMessage = "Saved"
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(2))
                savedMessage = nil
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
