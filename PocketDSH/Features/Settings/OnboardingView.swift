import SwiftUI

struct OnboardingView: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var appState: AppState
    @EnvironmentObject private var keychain: KeychainStore

    @State private var apiKey = ""
    @State private var revealKey = false
    @State private var errorMessage: String?
    @FocusState private var focused: Bool

    var body: some View {
        NavigationStack {
            ScrollView(showsIndicators: false) {
                VStack(alignment: .leading, spacing: 22) {
                    brand
                    explanation
                    keyField
                    actions
                    localOnly
                }
                .padding(PocketSpacing.xl)
                .padding(.top, PocketSpacing.md)
            }
            .background(PocketTheme.background)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Later") { dismiss() }
                        .foregroundStyle(PocketTheme.secondaryText)
                }
            }
        }
        .presentationDetents([.large])
        .presentationDragIndicator(.visible)
    }

    private var brand: some View {
        VStack(alignment: .leading, spacing: 12) {
            ZStack {
                RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                    .fill(PocketTheme.accent)
                    .frame(width: 54, height: 54)
                Image(systemName: "chevron.left.forwardslash.chevron.right")
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(.white)
            }

            Text("Connect the model")
                .font(PocketTheme.displayFont(size: 36))
                .foregroundStyle(PocketTheme.primaryText)
                .fixedSize(horizontal: false, vertical: true)

            Text("PocketDSH keeps your projects on this iPhone. The model reasons over them through the DeepSeek API.")
                .font(.subheadline)
                .foregroundStyle(PocketTheme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var explanation: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("API Key only", systemImage: "key")
                .font(.caption.weight(.semibold))
                .foregroundStyle(PocketTheme.accent)
            Text("PocketDSH does not collect your DeepSeek password and does not provide a fake account login. Use a DeepSeek API key; it is stored in the iOS Keychain.")
                .font(.caption)
                .foregroundStyle(PocketTheme.secondaryText)
        }
        .padding(PocketSpacing.md)
        .background {
            RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                .fill(PocketTheme.elevatedSurface)
        }
    }

    private var keyField: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("DeepSeek API Key")
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(PocketTheme.primaryText)
                Spacer()
                Link("Get a key", destination: URL(string: "https://platform.deepseek.com/api_keys")!)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.accent)
            }

            HStack(spacing: 8) {
                Group {
                    if revealKey {
                        TextField("sk-…", text: $apiKey)
                    } else {
                        SecureField("sk-…", text: $apiKey)
                    }
                }
                .font(.system(.body, design: .monospaced))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($focused)
                .padding(13)
                .background {
                    RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                        .fill(PocketTheme.surface)
                }
                .overlay {
                    RoundedRectangle(cornerRadius: PocketRadius.md, style: .continuous)
                        .stroke(PocketTheme.stroke, lineWidth: 1)
                }

                Button {
                    revealKey.toggle()
                } label: {
                    Image(systemName: revealKey ? "eye.slash" : "eye")
                        .foregroundStyle(PocketTheme.secondaryText)
                        .frame(width: 40, height: 40)
                }
                .buttonStyle(.plain)
            }

            if let errorMessage {
                Text(errorMessage)
                    .font(.caption)
                    .foregroundStyle(PocketTheme.red)
            }
        }
    }

    private var actions: some View {
        VStack(spacing: 8) {
            Button {
                save()
            } label: {
                Text("Save and start")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(PrimaryButtonStyle())
            .disabled(apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

            Button("Use local workspace without a key") {
                dismiss()
            }
            .font(.caption)
            .foregroundStyle(PocketTheme.secondaryText)
            .frame(maxWidth: .infinity)
            .padding(.top, 3)
        }
    }

    private var localOnly: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "iphone")
                .foregroundStyle(PocketTheme.secondaryText)
            Text("Files, editing and local plugins keep working without a key. Only model calls need the API.")
                .font(.caption2)
                .foregroundStyle(PocketTheme.secondaryText)
        }
        .padding(.top, PocketSpacing.xs)
    }

    private func save() {
        do {
            try keychain.save(apiKey)
            appState.showOnboarding = false
            dismiss()
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
