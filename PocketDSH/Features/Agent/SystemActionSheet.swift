import MessageUI
import UserNotifications
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
                    .font(PocketTheme.displayFont(size: 30))
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

                case .notification(let title, let body, let fireAt):
                    Button {
                        Task { await scheduleNotification(title: title, body: body, fireAt: fireAt) }
                    } label: {
                        Label(action.confirmTitle, systemImage: "bell.badge.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(PrimaryButtonStyle())

                    Button("Cancel") {
                        systemActions.clear()
                        dismiss()
                    }
                    .buttonStyle(SecondaryButtonStyle())
                    .frame(maxWidth: .infinity)
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

    /// Schedules the notification the user just approved.
    ///
    /// Permission is requested here rather than when the tool was called: the prompt should
    /// arrive attached to a specific, already-approved action, not to the model's idea for one.
    /// A refusal is reported rather than swallowed, because "nothing happened" is the one
    /// outcome a person cannot diagnose.
    private func scheduleNotification(title: String, body: String, fireAt: Date) async {
        let center = UNUserNotificationCenter.current()
        let granted: Bool
        do {
            granted = try await center.requestAuthorization(options: [.alert, .sound])
        } catch {
            systemActions.lastOutcome = "Notification permission failed: \(error.localizedDescription)"
            systemActions.clear()
            dismiss()
            return
        }
        guard granted else {
            systemActions.lastOutcome = "Notification permission was not granted, so nothing was scheduled."
            systemActions.clear()
            dismiss()
            return
        }

        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default

        let interval = max(1, fireAt.timeIntervalSinceNow)
        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: UNTimeIntervalNotificationTrigger(timeInterval: interval, repeats: false)
        )
        do {
            try await center.add(request)
            systemActions.lastOutcome = "Scheduled \"\(title)\" for \(fireAt.formatted(date: .abbreviated, time: .shortened))."
        } catch {
            systemActions.lastOutcome = "Could not schedule the notification: \(error.localizedDescription)"
        }
        systemActions.clear()
        dismiss()
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
