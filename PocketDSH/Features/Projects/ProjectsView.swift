import SwiftUI
import UniformTypeIdentifiers
import PocketDSHCore

struct ProjectsView: View {
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var projectStore: ProjectStore

    @State private var newProjectName = ""
    @State private var showNewProject = false
    @State private var showSettings = false
    @State private var showImporter = false
    @State private var importMode: ImportMode = .copy
    @State private var projectToDelete: ProjectRecord?

    var body: some View {
        NavigationStack {
            ZStack {
                PocketTheme.background
                    .ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 0) {
                        topBar
                        intro
                        actionRow
                        Divider()
                            .overlay(PocketTheme.stroke.opacity(0.8))
                            .padding(.vertical, PocketSpacing.xl)
                        projectList
                        footer
                    }
                    .padding(.horizontal, PocketSpacing.xl)
                    .padding(.top, PocketSpacing.md)
                    .padding(.bottom, PocketSpacing.xxl)
                }
            }
            .toolbar(.hidden, for: .navigationBar)
            .sheet(isPresented: $showSettings) {
                SettingsView()
                    .environmentObject(appState.settings)
                    .environmentObject(appState.runtime)
                    .environmentObject(appState.keychain)
                    .environmentObject(appState.pluginStore)
            }
            .alert("New project", isPresented: $showNewProject) {
                TextField("Project name", text: $newProjectName)
                Button("Create") { createProject() }
                Button("Cancel", role: .cancel) { newProjectName = "" }
            } message: {
                Text("A new workspace will be created inside this iPhone app.")
            }
            .fileImporter(
                isPresented: $showImporter,
                allowedContentTypes: [.folder]
            ) { result in
                handleImport(result)
            }
            .confirmationDialog(
                "Delete project?",
                isPresented: Binding(
                    get: { projectToDelete != nil },
                    set: { if !$0 { projectToDelete = nil } }
                ),
                titleVisibility: .visible
            ) {
                Button("Delete project", role: .destructive) {
                    if let project = projectToDelete {
                        projectStore.delete(project)
                        projectToDelete = nil
                    }
                }
                Button("Cancel", role: .cancel) { projectToDelete = nil }
            } message: {
                if let project = projectToDelete {
                    Text(
                        project.kind == .managed
                            ? "The managed workspace and its backups will be removed. External folders are never deleted, only detached."
                            : "The external folder stays untouched on disk; only PocketDSH's reference is removed."
                    )
                }
            }
        }
    }

    private var topBar: some View {
        HStack(spacing: 10) {
            ZStack {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(PocketTheme.accent)
                    .frame(width: 28, height: 28)
                Image(systemName: "chevron.left.forwardslash.chevron.right")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundStyle(.white)
            }

            Text("POCKETDSH / LOCAL")
                .font(.caption2.weight(.semibold).monospaced())
                .tracking(1.1)
                .foregroundStyle(PocketTheme.secondaryText)

            Spacer()

            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 17, weight: .regular))
                    .foregroundStyle(PocketTheme.secondaryText)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Settings")
        }
    }

    private var intro: some View {
        VStack(alignment: .leading, spacing: PocketSpacing.sm) {
            // A 42pt serif flatters a Latin heading and sets Chinese heavily, so
            // the face comes from the theme rather than being fixed here.
            Text("Local workspace")
                .font(PocketTheme.displayFont(size: 40))
                .foregroundStyle(PocketTheme.primaryText)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, PocketSpacing.xl)

            Text("A coding agent that lives on your iPhone. Projects stay here; the model only reasons over them.")
                .font(.subheadline)
                .foregroundStyle(PocketTheme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: PocketSpacing.sm) {
                StatusPill(text: "On-device files", tint: PocketTheme.accent)
                StatusPill(text: "Local copy", tint: PocketTheme.secondaryText)
            }
            .padding(.top, PocketSpacing.xxs)
        }
    }

    private var actionRow: some View {
        HStack(spacing: 8) {
            Button {
                newProjectName = ""
                showNewProject = true
            } label: {
                Label("New project", systemImage: "plus")
            }
            .buttonStyle(PrimaryButtonStyle())

            Menu {
                Button {
                    beginImport(.copy)
                } label: {
                    Label("Import folder", systemImage: "square.and.arrow.down")
                }
                Button {
                    beginImport(.external)
                } label: {
                    Label("Open authorized folder", systemImage: "folder.badge.plus")
                }
            } label: {
                Label("More", systemImage: "ellipsis")
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(PocketTheme.primaryText)
                    .padding(.horizontal, PocketSpacing.md)
                    .padding(.vertical, PocketSpacing.md)
                    .overlay {
                        RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                            .stroke(PocketTheme.stroke, lineWidth: 1)
                    }
            }
            .buttonStyle(.plain)
        }
        .padding(.top, PocketSpacing.lg)
    }

    private var projectList: some View {
        VStack(alignment: .leading, spacing: PocketSpacing.sm) {
            HStack(alignment: .firstTextBaseline) {
                Text("Projects")
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                Spacer()
                Text("\(projectStore.projects.count)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(PocketTheme.secondaryText)
            }

            if projectStore.projects.isEmpty {
                emptyProjects
                    .pocketCard(padding: PocketSpacing.lg)
            } else {
                // The rows sit inside one card rather than floating on the
                // canvas: a plain list on a gradient has no grouping at all,
                // which is what made this screen read as unfinished.
                VStack(spacing: 0) {
                    ForEach(projectStore.projects) { project in
                        Button {
                            appState.open(project)
                        } label: {
                            ProjectRow(project: project)
                                .padding(.horizontal, PocketSpacing.lg)
                        }
                        .buttonStyle(.plain)
                        .contextMenu {
                            Button(role: .destructive) {
                                projectToDelete = project
                            } label: {
                                Label("Remove project", systemImage: "trash")
                            }
                        }

                        if project.id != projectStore.projects.last?.id {
                            Divider()
                                .overlay(PocketTheme.stroke.opacity(0.6))
                                .padding(.leading, PocketSpacing.lg)
                        }
                    }
                }
                .pocketCard(padding: 0)
            }
        }
    }

    private var emptyProjects: some View {
        VStack(alignment: .leading, spacing: PocketSpacing.sm) {
            Text("No workspace yet.")
                .font(.body.weight(.medium))
                .foregroundStyle(PocketTheme.primaryText)
            Text("Create a project or import a folder. Nothing leaves this iPhone unless you export it.")
                .font(.subheadline)
                .foregroundStyle(PocketTheme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)
            Button("Create first project") {
                newProjectName = ""
                showNewProject = true
            }
            .buttonStyle(PrimaryButtonStyle())
            .padding(.top, PocketSpacing.xs)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var footer: some View {
        HStack(spacing: 6) {
            Image(systemName: "iphone")
            Text("No remote execution. Model inference uses DeepSeek over HTTPS.")
        }
        .font(.caption2)
        .foregroundStyle(PocketTheme.secondaryText)
        .fixedSize(horizontal: false, vertical: true)
        .padding(.top, PocketSpacing.xl)
    }

    private func createProject() {
        do {
            let project = try projectStore.createProject(named: newProjectName)
            newProjectName = ""
            appState.open(project)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    private func beginImport(_ mode: ImportMode) {
        importMode = mode
        showImporter = true
    }

    private func handleImport(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            do {
                let project: ProjectRecord
                switch importMode {
                case .copy:
                    project = try projectStore.importManagedProject(from: url)
                case .external:
                    project = try projectStore.addExternalProject(from: url)
                }
                appState.open(project)
            } catch {
                appState.errorMessage = error.localizedDescription
            }
        case .failure(let error):
            appState.errorMessage = error.localizedDescription
        }
    }

    private enum ImportMode {
        case copy
        case external
    }
}

private struct ProjectRow: View {
    let project: ProjectRecord

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: project.kind == .managed ? "folder" : "externaldrive")
                .font(.system(size: 17, weight: .regular))
                .foregroundStyle(PocketTheme.secondaryText)
                .frame(width: 28, alignment: .leading)

            VStack(alignment: .leading, spacing: 4) {
                Text(project.name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(PocketTheme.primaryText)
                    .lineLimit(1)

                HStack(spacing: 6) {
                    Text(project.kind == .managed ? LocalizedStringKey("Local copy") : LocalizedStringKey("Authorized folder"))
                    Text("·")
                    Text(project.lastOpenedAt.formatted(.relative(presentation: .named)))
                }
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)
            }

            Spacer(minLength: 8)

            Image(systemName: "chevron.right")
                .font(.caption2.weight(.semibold))
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.6))
        }
        .padding(.vertical, PocketSpacing.md)
        .contentShape(Rectangle())
    }
}
