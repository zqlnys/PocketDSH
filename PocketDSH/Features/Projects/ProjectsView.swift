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
                    VStack(alignment: .leading, spacing: 22) {
                        header
                        hero
                        actionRow
                        projectList
                        footer
                    }
                    .padding(.horizontal, 20)
                    .padding(.top, 10)
                    .padding(.bottom, 36)
                }
            }
            .toolbar(.hidden, for: .navigationBar)
            .sheet(isPresented: $showSettings) {
                SettingsView()
                    .environmentObject(appState.settings)
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

    private var header: some View {
        HStack(alignment: .top, spacing: 14) {
            VStack(alignment: .leading, spacing: 5) {
                Text("POCKET / WORKSPACE")
                    .font(.caption.weight(.bold))
                    .tracking(1.4)
                    .foregroundStyle(PocketTheme.accent)

                Text("PocketDSH")
                    .font(.system(size: 36, weight: .bold, design: .rounded))
                    .foregroundStyle(PocketTheme.primaryText)

                Text("A coding agent that lives on your iPhone.")
                    .font(.subheadline)
                    .foregroundStyle(PocketTheme.secondaryText)
            }

            Spacer(minLength: 0)

            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(PocketTheme.surface))
                    .overlay(Circle().stroke(PocketTheme.stroke, lineWidth: 1))
            }
            .buttonStyle(PressableButtonStyle())
            .accessibilityLabel("Settings")
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                StatusPill(text: "Local first", tint: PocketTheme.accent)
                Spacer()
                Text("iOS 17+")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(.white.opacity(0.55))
            }

            Text("The project is not a chat attachment.")
                .font(.system(size: 23, weight: .bold, design: .rounded))
                .foregroundStyle(.white)

            Text("Files are read and written on this device. The agent uses real tools, records diffs, and keeps recovery backups.")
                .font(.subheadline)
                .foregroundStyle(.white.opacity(0.72))
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 16) {
                Label("\(projectStore.projects.count) projects", systemImage: "folder.fill")
                Label("Lua 5.4", systemImage: "terminal.fill")
            }
            .font(.caption.weight(.medium))
            .foregroundStyle(.white.opacity(0.68))
            .padding(.top, 2)
        }
        .padding(20)
        .background(PocketTheme.headerGradient)
        .clipShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
        .overlay(alignment: .topTrailing) {
            Image(systemName: "chevron.left.forwardslash.chevron.right")
                .font(.system(size: 82, weight: .bold))
                .foregroundStyle(.white.opacity(0.055))
                .offset(x: 18, y: -10)
        }
    }

    private var actionRow: some View {
        VStack(spacing: 10) {
            Button {
                newProjectName = ""
                showNewProject = true
            } label: {
                Label("New project", systemImage: "plus")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(PrimaryButtonStyle())

            HStack(spacing: 10) {
                Button {
                    beginImport(.copy)
                } label: {
                    Label("Import folder", systemImage: "square.and.arrow.down")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(SecondaryButtonStyle())

                Button {
                    beginImport(.external)
                } label: {
                    Label("Open folder", systemImage: "folder.badge.plus")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(SecondaryButtonStyle())
            }
        }
    }

    private var projectList: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                SectionLabel(title: "Your workspaces")
                Spacer()
                if !projectStore.projects.isEmpty {
                    Text("\(projectStore.projects.count)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }

            if projectStore.projects.isEmpty {
                EmptyState(
                    icon: "folder.badge.plus",
                    title: "No workspace yet",
                    message: "Create a project or import a folder. Everything stays on this iPhone unless you export it.",
                    actionTitle: "Create first project"
                ) {
                    newProjectName = ""
                    showNewProject = true
                }
            } else {
                ForEach(projectStore.projects) { project in
                    Button {
                        appState.open(project)
                    } label: {
                        ProjectCard(project: project)
                    }
                    .buttonStyle(PressableButtonStyle())
                    .contextMenu {
                        Button(role: .destructive) {
                            projectToDelete = project
                        } label: {
                            Label("Remove project", systemImage: "trash")
                        }
                    }
                }
            }
        }
    }

    private var footer: some View {
        HStack(spacing: 6) {
            Image(systemName: "iphone")
            Text("No remote execution in this build. Model inference uses DeepSeek over HTTPS.")
        }
        .font(.caption2)
        .foregroundStyle(PocketTheme.secondaryText)
        .padding(.top, 2)
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

private struct ProjectCard: View {
    let project: ProjectRecord

    var body: some View {
        HStack(spacing: 14) {
            IconTile(
                systemName: project.kind == .managed ? "folder.fill" : "externaldrive.fill",
                tint: project.kind == .managed ? PocketTheme.accent : PocketTheme.blue,
                size: 48
            )

            VStack(alignment: .leading, spacing: 4) {
                Text(project.name)
                    .font(.body.weight(.semibold))
                    .foregroundStyle(PocketTheme.primaryText)
                    .lineLimit(1)

                HStack(spacing: 7) {
                    Text(project.kind == .managed ? "Local copy" : "Authorized folder")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                    Text("·")
                        .foregroundStyle(PocketTheme.secondaryText)
                    Text(project.lastOpenedAt.formatted(.relative(presentation: .named)))
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }

            Spacer(minLength: 8)

            Image(systemName: "chevron.right")
                .font(.caption.weight(.bold))
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.7))
        }
        .padding(16)
        .background {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .fill(PocketTheme.surface)
        }
        .overlay {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .stroke(PocketTheme.stroke.opacity(0.65), lineWidth: 1)
        }
    }
}
