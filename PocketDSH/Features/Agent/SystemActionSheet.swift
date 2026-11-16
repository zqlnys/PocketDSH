import MessageUI
import SwiftUI
import UIKit

struct SystemActionSheet: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var systemActions: SystemActionStore
    let action: PendingSystemAction

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 18) {
                Text(NSLocalizedString(action.title, comment: ""))
                    .font(.system(size: 30, weight: .medium, design: .serif))
                    .foregroundStyle(PocketTheme.primaryText)

                Text(action.detail)
                    .font(.body)
                    .foregroundStyle(PocketTheme.secondaryText)
                    .textSelection(.enabled)

                Spacer(minLength: 0)

                switch action.kind {
                case .call(let number):
                    Button {
                        openDialer(number)
                    } label: {
                        Label("Open dialer", systemImage: "phone.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(PrimaryButtonStyle())

                    Button("Cancel") {
                        systemActions.clear()
                        dismiss()
                    }
                    .buttonStyle(SecondaryButtonStyle())
                    .frame(maxWidth: .infinity)

                case .message(let recipient, let body):
                    MessageComposer(
                        recipient: recipient,
                        body: body
                    ) {
                        systemActions.clear()
                        dismiss()
                    }
                    .frame(maxWidth: .infinity, maxHeight: 420)
                }
            }
            .padding(PocketSpacing.xl)
            .background(PocketTheme.background)
            .navigationTitle("System action")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") {
                        systemActions.clear()
                        dismiss()
                    }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func openDialer(_ number: String) {
        let cleaned = number.filter { $0.isNumber || $0 == "+" }
        guard let url = URL(string: "tel://\(cleaned)") else { return }
        UIApplication.shared.open(url)
        systemActions.clear()
        dismiss()
    }
}

private struct MessageComposer: UIViewControllerRepresentable {
    let recipient: String
    let body: String
    let onFinish: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onFinish: onFinish)
    }

    func makeUIViewController(context: Context) -> MFMessageComposeViewController {
        let controller = MFMessageComposeViewController()
        controller.recipients = [recipient]
        controller.body = body
        controller.messageComposeDelegate = context.coordinator
        return controller
    }

    func updateUIViewController(_ uiViewController: MFMessageComposeViewController, context: Context) {}

    final class Coordinator: NSObject, MFMessageComposeViewControllerDelegate {
        let onFinish: () -> Void

        init(onFinish: @escaping () -> Void) {
            self.onFinish = onFinish
        }

        func messageComposeViewController(
            _ controller: MFMessageComposeViewController,
            didFinishWith result: MessageComposeResult
        ) {
            controller.dismiss(animated: true)
            onFinish()
        }
    }
}
