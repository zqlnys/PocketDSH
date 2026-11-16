import SwiftUI
import PocketDSHCore

struct SettingsView: View {
    @EnvironmentObject private var settings: SettingsStore
    @EnvironmentObject private var keychain: KeychainStore
    @EnvironmentObject private var pluginStore: PluginStore
    @EnvironmentObject private var runtime: RuntimeCapabilityStore

    @State private var apiKeyDraft = ""
    @State private var revealKey = false
    @State private var keyMessage: String?
    @State private var keyError: String?
    @State private var showTierCSmoke = false

    var body: some View {
        NavigationStack {
            ZStack {
                PocketTheme.background
                    .ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 18) {
                        apiKeyCard
                        modelCard
                        languageCard
                        pluginsCard
                        runtimeCard
                        jitCard
                        tierCCard
                        compatibilityCard
                        aboutCard
                    }
                    .padding(.horizontal, 20)
                    .padding(.top, 10)
                    .padding(.bottom, 36)
                }
            }
            .sheet(isPresented: $showTierCSmoke) {
                TierCSmokeView()
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .onAppear(perform: loadKey)
        }
    }

    private var apiKeyCard: some View {
        VStack(alignment: .leading, spacing: 13) {
            HStack {
                SectionLabel(title: "DeepSeek API")
                Spacer()
                StatusPill(
                    text: keychain.hasKey ? "Key stored" : "No key",
                    tint: keychain.hasKey ? PocketTheme.accent : PocketTheme.amber
                )
            }

            Text("The key is saved in the iOS Keychain. It is never written into a project file or conversation.")
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)

            HStack(spacing: 8) {
                Group {
                    if revealKey {
                        TextField("sk-…", text: $apiKeyDraft)
                    } else {
                        SecureField("sk-…", text: $apiKeyDraft)
                    }
                }
                .font(.system(.body, design: .monospaced))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .padding(12)
                .background {
                    RoundedRectangle(cornerRadius: 13, style: .continuous)
                        .fill(PocketTheme.elevatedSurface)
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 13, style: .continuous)
                        .stroke(PocketTheme.stroke.opacity(0.6), lineWidth: 1)
                }

                Button {
                    revealKey.toggle()
                } label: {
                    Image(systemName: revealKey ? "eye.slash" : "eye")
                        .foregroundStyle(PocketTheme.secondaryText)
                        .frame(width: 36, height: 36)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(revealKey ? "Hide key" : "Show key")
            }

            HStack(spacing: 10) {
                Button("Save key") {
                    saveKey()
                }
                .buttonStyle(PrimaryButtonStyle())

                Button("Remove") {
                    removeKey()
                }
                .buttonStyle(SecondaryButtonStyle())
                .disabled(!keychain.hasKey)
            }

            if let keyMessage {
                Text(keyMessage)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.accent)
            }
            if let keyError {
                Text(keyError)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.red)
            }
        }
        .pocketCard()
    }

    private var modelCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            SectionLabel(title: "Agent")

            HStack {
                Text("Model")
                    .font(.subheadline)
                    .foregroundStyle(PocketTheme.primaryText)
                Spacer()
                Picker("Model", selection: $settings.model) {
                    ForEach(settings.availableModels, id: \.self) { model in
                        Text(model).tag(model)
                    }
                }
                .pickerStyle(.menu)
                .tint(PocketTheme.accent)
            }

            Divider()
                .overlay(PocketTheme.stroke.opacity(0.6))

            HStack {
                Text("Reasoning effort")
                    .font(.subheadline)
                    .foregroundStyle(PocketTheme.primaryText)
                Spacer()
                Picker("Reasoning effort", selection: $settings.reasoningEffort) {
                    ForEach(ReasoningEffort.allCases) { effort in
                        Text(effort.title).tag(effort)
                    }
                }
                .pickerStyle(.menu)
                .tint(PocketTheme.accent)
            }

            Toggle(isOn: $settings.thinkingEnabled) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Thinking mode")
                        .font(.subheadline)
                        .foregroundStyle(PocketTheme.primaryText)
                    Text("Off by default for predictable tool use. DeepSeek thinking mode may add latency.")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
            .tint(PocketTheme.accent)

            Divider()
                .overlay(PocketTheme.stroke.opacity(0.6))

            Stepper(value: $settings.maxSteps, in: 3...20) {
                HStack {
                    Text("Maximum steps")
                        .font(.subheadline)
                        .foregroundStyle(PocketTheme.primaryText)
                    Spacer()
                    Text("\(settings.maxSteps)")
                        .font(.subheadline.monospacedDigit())
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
            .tint(PocketTheme.accent)

            Toggle(isOn: $settings.showsToolDetails) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Show tool output")
                        .font(.subheadline)
                        .foregroundStyle(PocketTheme.primaryText)
                    Text("Raw stdout, diffs and file results in the task log.")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
            .tint(PocketTheme.accent)
        }
        .pocketCard()
    }

    private var languageCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionLabel(title: "Language")
            Picker("Language", selection: $settings.language) {
                ForEach(AppLanguage.allCases) { language in
                    Text(language.title).tag(language)
                }
            }
            .pickerStyle(.segmented)
        }
        .pocketCard()
    }

    private var pluginsCard: some View {
        NavigationLink {
            PluginsView()
        } label: {
            HStack(spacing: 12) {
                IconTile(systemName: "puzzlepiece.extension.fill", tint: PocketTheme.violet, size: 40)
                VStack(alignment: .leading, spacing: 3) {
                    Text("Plugins")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(PocketTheme.primaryText)
                    Text("\(pluginStore.plugins.count) installed · \(pluginStore.plugins.filter(\.enabled).count) enabled")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(PocketTheme.secondaryText)
            }
            .padding(14)
            .background {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .fill(PocketTheme.surface)
            }
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(PocketTheme.stroke.opacity(0.65), lineWidth: 1)
            }
        }
        .buttonStyle(.plain)
    }

    private var runtimeCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionLabel(title: "Local runtime")

            HStack(spacing: 10) {
                IconTile(systemName: "terminal.fill", tint: PocketTheme.accent, size: 40)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Lua 5.4 · embedded")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(PocketTheme.primaryText)
                    Text("Runs in this app process, no JIT, no subprocess.")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }

            Divider()
                .overlay(PocketTheme.stroke.opacity(0.6))

            VStack(alignment: .leading, spacing: 6) {
                runtimeLine("Available", "base, string, table, math, utf8, coroutine, print")
                runtimeLine("Blocked", "io, os, package, debug, dofile, loadfile")
                runtimeLine("Not available", "shell, fork/exec, Python, Node, network, Xcode")
            }

            Text("iOS apps cannot launch arbitrary system binaries. Unsupported operations fail explicitly; nothing is silently forwarded to a server.")
                .font(.caption2)
                .foregroundStyle(PocketTheme.secondaryText)
        }
        .pocketCard()
    }

    private var jitCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                SectionLabel(title: "JIT & Runtime")
                Spacer()
                StatusPill(
                    text: runtime.jit.isAvailable ? "JIT detected" : "No JIT",
                    tint: runtime.jit.isAvailable ? PocketTheme.accent : PocketTheme.amber
                )
            }

            Text(runtime.detail)
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)

            Toggle(isOn: $runtime.preferJIT) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Prefer JIT acceleration")
                        .font(.subheadline)
                        .foregroundStyle(PocketTheme.primaryText)
                    Text("A toggle cannot grant JIT. Your signature must provide the entitlement; this only controls the preference.")
                        .font(.caption)
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
            .tint(PocketTheme.accent)

            Picker("Runtime preference", selection: $runtime.preferredTier) {
                ForEach(RuntimeTier.allCases) { tier in
                    Text(tier.title).tag(tier)
                }
            }
            .pickerStyle(.menu)
            .tint(PocketTheme.accent)

            LabeledContent("JIT probe", value: runtime.jit.entitlementSummary)
                .font(.caption)
            LabeledContent("Active backend", value: runtime.activeTier.title)
                .font(.caption)
            LabeledContent("Automatic preference", value: runtime.automaticTier.title)
                .font(.caption)

            if runtime.wantsTierC {
                Banner(
                    icon: "exclamationmark.triangle",
                    title: "Tier C is experimental",
                    message: "The iSH Alpine backend runs a persistent shell, but Tier C is still experimental. Tier A remains the default runtime.",
                    tint: PocketTheme.amber
                )
            }
        }
        .pocketCard()
    }

    private var tierCCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                SectionLabel(title: "Tier C · Experimental")
                Spacer()
                StatusPill(text: "Linked", tint: PocketTheme.amber)
            }

            Text("iSH Alpine runs a persistent shell inside this app. run_linux executes in that shell, and the active project is mounted read-write at /workspace, so guest commands see the same files the Swift tools do. Tier C is still experimental and is not the default runtime.")
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)

            Text("GPLv3: distribution requires source/license compliance.")
                .font(.caption2)
                .foregroundStyle(PocketTheme.amber)

            Button {
                showTierCSmoke = true
            } label: {
                Label("Run Tier C smoke test", systemImage: "terminal")
            }
            .buttonStyle(SecondaryButtonStyle())
            .frame(maxWidth: .infinity)
        }
        .pocketCard()
    }

    private var compatibilityCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionLabel(title: "DSH compatibility")

            Text("This build implements a native tool loop and a DSH-inspired filesystem subset. It does not embed the Node/Cordis DSH runtime, so arbitrary DSH plugins are not loadable.")
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)

            Text("Mapped now: list_files, read_file, write_file, search_files, apply_patch, run_lua.\nNot yet: Cordis plugins, subagents, persistent Bash/PowerShell, remote MCP servers.")
                .font(.caption2.monospaced())
                .foregroundStyle(PocketTheme.primaryText.opacity(0.85))
        }
        .pocketCard()
    }

    private var aboutCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel(title: "About")
            Text("PocketDSH \(version)")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(PocketTheme.primaryText)
            Text("Local-first iOS coding agent. Model inference uses the DeepSeek API; your files stay on this phone.")
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)
            Text("No analytics · no remote workspace · API key in Keychain")
                .font(.caption2)
                .foregroundStyle(PocketTheme.secondaryText.opacity(0.8))
        }
        .pocketCard()
    }

    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.1.0"
    }

    private func runtimeLine(_ label: String, _ value: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Text(label)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(PocketTheme.secondaryText)
                .frame(width: 78, alignment: .leading)
            Text(value)
                .font(.caption2.monospaced())
                .foregroundStyle(PocketTheme.primaryText.opacity(0.85))
            Spacer(minLength: 0)
        }
    }

    private func loadKey() {
        apiKeyDraft = keychain.apiKey() ?? ""
    }

    private func saveKey() {
        keyMessage = nil
        keyError = nil
        do {
            try keychain.save(apiKeyDraft)
            keyMessage = keychain.hasKey ? "Saved to Keychain." : "Key removed."
        } catch {
            keyError = error.localizedDescription
        }
    }

    private func removeKey() {
        keyMessage = nil
        keyError = nil
        do {
            try keychain.delete()
            apiKeyDraft = ""
            keyMessage = "Removed from Keychain."
        } catch {
            keyError = error.localizedDescription
        }
    }
}
