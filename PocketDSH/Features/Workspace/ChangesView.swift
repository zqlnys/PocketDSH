import SwiftUI
import PocketDSHCore

struct ChangesView: View {
    @EnvironmentObject private var appState: AppState
    @ObservedObject var session: WorkspaceSession

    @State private var records: [ChangeRecord] = []
    @State private var selectedChange: ChangeRecord?
    @State private var restoreTarget: ChangeRecord?
    @State private var errorMessage: String?

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Banner(
                        icon: "clock.arrow.circlepath",
                        title: "Every overwrite has a recovery copy",
                        message: "Files changed by the agent or editor are backed up outside the workspace. Restore creates another recorded change, so recovery is reversible too.",
                        tint: PocketTheme.accent
                    )
                    .listRowInsets(EdgeInsets(top: 8, leading: 20, bottom: 8, trailing: 20))
                    .listRowBackground(Color.clear)
                }

                Section("Recent changes") {
                    if let notice = historyNotice {
                        // An unreadable history is not an empty one, and saying "no changes yet"
                        // about it is a claim the app has no right to make.
                        ContentUnavailableView(
                            "History unavailable",
                            systemImage: "exclamationmark.triangle",
                            description: Text(notice)
                        )
                        .listRowBackground(Color.clear)
                    } else if records.isEmpty {
                        ContentUnavailableView(
                            "No changes yet",
                            systemImage: "clock",
                            description: Text("Edits and agent writes will appear here with their diffs and backups.")
                        )
                        .listRowBackground(Color.clear)
                    } else {
                        ForEach(records.reversed()) { record in
                            Button {
                                selectedChange = record
                            } label: {
                                ChangeRow(record: record)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .background(PocketTheme.background)
            .navigationTitle("Changes")
            .navigationBarTitleDisplayMode(.inline)
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
                    Button {
                        load()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .accessibilityLabel("Refresh changes")
                }
            }
            .onAppear(perform: load)
            .sheet(item: $selectedChange) { record in
                NavigationStack {
                    RawDiffView(diff: record.diff, path: record.path)
                        .toolbar {
                            ToolbarItem(placement: .cancellationAction) {
                                Button("Done") { selectedChange = nil }
                            }
                            ToolbarItem(placement: .confirmationAction) {
                                Button("Restore") {
                                    restoreTarget = record
                                }
                                .disabled(record.backupPath == nil)
                            }
                        }
                }
                .presentationDetents([.large])
            }
            .confirmationDialog(
                "Restore this version?",
                isPresented: Binding(
                    get: { restoreTarget != nil },
                    set: { if !$0 { restoreTarget = nil } }
                ),
                titleVisibility: .visible
            ) {
                Button("Restore file", role: .destructive) {
                    restore()
                }
                Button("Cancel", role: .cancel) { restoreTarget = nil }
            } message: {
                Text("The current file will be backed up before the selected version is restored.")
            }
            .alert(
                "Changes",
                isPresented: Binding(
                    get: { errorMessage != nil },
                    set: { if !$0 { errorMessage = nil } }
                )
            ) {
                Button("OK", role: .cancel) { errorMessage = nil }
            } message: {
                Text(errorMessage ?? "")
            }
        }
    }

    private func load() {
        let outcome = session.fileSystem.changesOutcome()
        records = outcome.records
        // Shown rather than swallowed: an empty list and an unreadable list look identical, and
        // only one of them means there is nothing to undo.
        historyNotice = outcome.failure
    }

    @State private var historyNotice: String?

    private func restore() {
        guard let target = restoreTarget else { return }
        do {
            try session.fileSystem.restore(target)
            restoreTarget = nil
            selectedChange = nil
            load()
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

private struct ChangeRow: View {
    let record: ChangeRecord

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            IconTile(
                systemName: icon,
                tint: tint,
                size: 38
            )

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(record.path)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(PocketTheme.primaryText)
                        .lineLimit(1)
                    if record.backupPath != nil {
                        Image(systemName: "arrow.uturn.backward.circle.fill")
                            .font(.caption2)
                            .foregroundStyle(PocketTheme.accent)
                    }
                }

                Text(record.summary)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.secondaryText)
                    .lineLimit(2)

                Text(record.timestamp.formatted(date: .abbreviated, time: .shortened))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(PocketTheme.secondaryText.opacity(0.8))
            }

            Spacer(minLength: 0)

            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.7))
                .padding(.top, PocketSpacing.xs)
        }
        .padding(.vertical, PocketSpacing.xs)
    }

    private var icon: String {
        switch record.kind {
        case .write: return "square.and.pencil"
        case .patch: return "plus.forwardslash.minus"
        case .editor: return "text.cursor"
        case .restore: return "arrow.uturn.backward"
        }
    }

    private var tint: Color {
        switch record.kind {
        case .write, .editor: return PocketTheme.amber
        case .patch: return PocketTheme.blue
        case .restore: return PocketTheme.accent
        }
    }
}
