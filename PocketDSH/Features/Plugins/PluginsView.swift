import SwiftUI
import UniformTypeIdentifiers
import PocketDSHCore

struct PluginsView: View {
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var pluginStore: PluginStore

    @State private var showImporter = false
    @State private var errorMessage: String?
    @State private var showExampleConfirm = false

    var body: some View {
        List {
            Section {
                Banner(
                    icon: "puzzlepiece.extension.fill",
                    title: "Custom Lua plugin runtime",
                    message: "This is a PocketDSH-specific plugin format. It is not a Cordis/Node DSH plugin runtime. Plugins are real Lua 5.4 bundles with declared tools and controlled workspace access.",
                    tint: PocketTheme.violet
                )
                .listRowInsets(EdgeInsets(top: 8, leading: 20, bottom: 8, trailing: 20))
                .listRowBackground(Color.clear)
            }

            Section("Install") {
                Button {
                    showExampleConfirm = true
                } label: {
                    Label("Install example: Workspace Inspector", systemImage: "sparkles")
                }

                Button {
                    showImporter = true
                } label: {
                    Label("Install plugin from folder", systemImage: "square.and.arrow.down")
                }
            }

            Section("Installed") {
                if pluginStore.plugins.isEmpty {
                    ContentUnavailableView(
                        "No plugins installed",
                        systemImage: "puzzlepiece",
                        description: Text("Install the example plugin to give the agent real workspace tools.")
                    )
                    .listRowBackground(Color.clear)
                } else {
                    ForEach(pluginStore.plugins) { plugin in
                        HStack(spacing: 12) {
                            IconTile(
                                systemName: "puzzlepiece.extension.fill",
                                tint: plugin.enabled ? PocketTheme.accent : PocketTheme.secondaryText,
                                size: 38
                            )

                            VStack(alignment: .leading, spacing: 3) {
                                Text(plugin.manifest.name)
                                    .font(.subheadline.weight(.semibold))
                                    .foregroundStyle(PocketTheme.primaryText)
                                Text("v\(plugin.manifest.version) · \(plugin.manifest.tools.count) tool(s)")
                                    .font(.caption2.monospaced())
                                    .foregroundStyle(PocketTheme.secondaryText)
                                if let error = plugin.lastError, !error.isEmpty {
                                    Text(error)
                                        .font(.caption2)
                                        .foregroundStyle(PocketTheme.red)
                                        .lineLimit(2)
                                }
                            }

                            Spacer(minLength: 4)

                            Toggle(
                                "",
                                isOn: Binding(
                                    get: { plugin.enabled },
                                    set: { setEnabled($0, for: plugin) }
                                )
                            )
                            .labelsHidden()
                            .tint(PocketTheme.accent)

                            NavigationLink {
                                PluginDetailView(pluginID: plugin.manifest.id)
                            } label: {
                                Image(systemName: "chevron.right")
                                    .font(.caption.weight(.semibold))
                                    .foregroundStyle(PocketTheme.secondaryText)
                            }
                            .fixedSize()
                        }
                        .padding(.vertical, 2)
                    }
                }
            }

            Section("Runtime") {
                LabeledContent("Language", value: "Lua 5.4.7")
                LabeledContent("Sandbox", value: "No io/os/package/debug")
                LabeledContent("Workspace", value: "Guarded host calls only")
                LabeledContent("Timeout", value: "5 seconds per tool")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Plugins")
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(
            isPresented: $showImporter,
            allowedContentTypes: [.folder]
        ) { result in
            install(result)
        }
        .alert("Plugins", isPresented: Binding(
            get: { errorMessage != nil },
            set: { if !$0 { errorMessage = nil } }
        )) {
            Button("OK", role: .cancel) { errorMessage = nil }
        } message: {
            Text(errorMessage ?? "")
        }
        .confirmationDialog(
            "Install the example plugin?",
            isPresented: $showExampleConfirm,
            titleVisibility: .visible
        ) {
            Button("Install example") {
                do {
                    _ = try pluginStore.installExample()
                    appState.activeSession?.pluginRuntime.reload()
                } catch {
                    errorMessage = error.localizedDescription
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Adds Workspace Inspector with workspace.read/workspace.write permissions and two real tools.")
        }
    }

    private func setEnabled(_ enabled: Bool, for plugin: InstalledPlugin) {
        pluginStore.setEnabled(enabled, for: plugin.manifest.id)
        appState.activeSession?.pluginRuntime.reload()
    }

    private func install(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            do {
                _ = try pluginStore.install(from: url)
                appState.activeSession?.pluginRuntime.reload()
            } catch {
                errorMessage = error.localizedDescription
            }
        case .failure(let error):
            errorMessage = error.localizedDescription
        }
    }
}

struct PluginDetailView: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var pluginStore: PluginStore
    let pluginID: String

    @State private var values: [String: JSONValue] = [:]
    @State private var errorMessage: String?
    @State private var showUninstall = false
    @State private var configLoaded = false

    private var plugin: InstalledPlugin? {
        pluginStore.plugin(id: pluginID)
    }

    var body: some View {
        Group {
            if let plugin {
                List {
                    Section {
                        HStack(spacing: 12) {
                            IconTile(systemName: "puzzlepiece.extension.fill", tint: PocketTheme.violet, size: 44)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(plugin.manifest.name)
                                    .font(.headline)
                                Text(plugin.manifest.description)
                                    .font(.caption)
                                    .foregroundStyle(PocketTheme.secondaryText)
                            }
                        }
                        LabeledContent("Version", value: plugin.manifest.version)
                        LabeledContent("API", value: "v\(plugin.manifest.apiVersion)")
                        LabeledContent("Author", value: plugin.manifest.author ?? "Unknown")
                        LabeledContent("Permissions", value: plugin.manifest.permissions.joined(separator: ", "))
                    }

                    Section("Tools") {
                        ForEach(plugin.manifest.tools, id: \.name) { tool in
                            VStack(alignment: .leading, spacing: 3) {
                                Text(tool.name)
                                    .font(.subheadline.weight(.semibold).monospaced())
                                Text(tool.description)
                                    .font(.caption)
                                    .foregroundStyle(PocketTheme.secondaryText)
                            }
                            .padding(.vertical, 2)
                        }
                    }

                    if !plugin.manifest.config.isEmpty {
                        Section("Configuration") {
                            ForEach(plugin.manifest.config) { field in
                                configField(field)
                            }
                            Button("Save configuration") {
                                saveConfig()
                            }
                            .buttonStyle(PrimaryButtonStyle())
                        }
                    }

                    Section("Logs") {
                        let entries = pluginStore.logs(for: pluginID)
                        if entries.isEmpty {
                            Text("No logs yet.")
                                .font(.caption)
                                .foregroundStyle(PocketTheme.secondaryText)
                        } else {
                            ForEach(Array(entries.suffix(30))) { entry in
                                HStack(alignment: .top, spacing: 8) {
                                    Text(entry.level.rawValue.uppercased())
                                        .font(.caption2.weight(.bold).monospaced())
                                        .foregroundStyle(logColor(entry.level))
                                        .frame(width: 56, alignment: .leading)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(entry.message)
                                            .font(.caption.monospaced())
                                            .textSelection(.enabled)
                                        Text(entry.timestamp.formatted(date: .omitted, time: .standard))
                                            .font(.caption2.monospacedDigit())
                                            .foregroundStyle(PocketTheme.secondaryText)
                                    }
                                }
                            }
                        }
                    }

                    Section {
                        Toggle(
                            "Enabled",
                            isOn: Binding(
                                get: { plugin.enabled },
                                set: { enabled in
                                    pluginStore.setEnabled(enabled, for: pluginID)
                                    appState.activeSession?.pluginRuntime.reload()
                                }
                            )
                        )
                        .tint(PocketTheme.accent)

                        Button("Uninstall plugin", role: .destructive) {
                            showUninstall = true
                        }
                    }
                }
                .listStyle(.insetGrouped)
                .navigationTitle("Plugin")
                .navigationBarTitleDisplayMode(.inline)
                .onAppear {
                    if !configLoaded {
                        values = plugin.config
                        configLoaded = true
                    }
                }
                .confirmationDialog(
                    "Uninstall \(plugin.manifest.name)?",
                    isPresented: $showUninstall,
                    titleVisibility: .visible
                ) {
                    Button("Uninstall", role: .destructive) {
                        pluginStore.uninstall(plugin)
                        appState.activeSession?.pluginRuntime.reload()
                        dismiss()
                    }
                    Button("Cancel", role: .cancel) {}
                } message: {
                    Text("The plugin folder and its logs are removed. Workspace files it already wrote are not deleted.")
                }
                .alert("Plugin", isPresented: Binding(
                    get: { errorMessage != nil },
                    set: { if !$0 { errorMessage = nil } }
                )) {
                    Button("OK", role: .cancel) { errorMessage = nil }
                } message: {
                    Text(errorMessage ?? "")
                }
            } else {
                ContentUnavailableView(
                    "Plugin not installed",
                    systemImage: "puzzlepiece",
                    description: Text("It may have been uninstalled.")
                )
            }
        }
    }

    @ViewBuilder
    private func configField(_ field: PluginConfigField) -> some View {
        switch field.type {
        case .string:
            TextField(
                field.title,
                text: Binding(
                    get: { values[field.key]?.stringValue ?? "" },
                    set: { values[field.key] = .string($0) }
                )
            )
        case .boolean:
            Toggle(
                field.title,
                isOn: Binding(
                    get: { values[field.key]?.boolValue ?? false },
                    set: { values[field.key] = .bool($0) }
                )
            )
        case .number:
            TextField(
                field.title,
                text: Binding(
                    get: {
                        if case .number(let value) = values[field.key] { return String(value) }
                        return ""
                    },
                    set: { values[field.key] = .number(Double($0) ?? 0) }
                )
            )
            .keyboardType(.decimalPad)
        }
    }

    private func saveConfig() {
        guard let plugin else { return }
        var merged = plugin.config
        for field in plugin.manifest.config {
            if let value = values[field.key] {
                merged[field.key] = value
            }
        }
        pluginStore.updateConfig(merged, for: pluginID)
        appState.activeSession?.pluginRuntime.reload()
        errorMessage = "Configuration saved."
    }

    private func logColor(_ level: PluginLogEntry.Level) -> Color {
        switch level {
        case .debug: return PocketTheme.secondaryText
        case .info: return PocketTheme.accent
        case .warning: return PocketTheme.amber
        case .error: return PocketTheme.red
        }
    }
}
