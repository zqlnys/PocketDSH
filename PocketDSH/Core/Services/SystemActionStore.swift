import Combine
import Foundation

/// Something the agent wants the system to do, staged for the person to approve.
///
/// The rule these types exist to enforce is that nothing reaches the system on the agent's
/// word alone. A tool call produces a `PendingSystemAction`; the user sees exactly what would
/// happen and taps once; only then does anything leave the app. Calls and messages worked this
/// way from the start, while `schedule_alert` scheduled a notification directly - asking for
/// permission to notify is not the same as asking whether this notification should exist.
struct PendingSystemAction: Identifiable, Equatable {
    enum Kind: Equatable {
        case call(number: String)
        /// A message staged for the system composer. The app never sends it itself.
        case message(recipient: String, body: String)
        /// A local notification, which is what an alarm is on iOS: an app cannot create a
        /// Clock alarm, but it can deliver a notification at a chosen time.
        case notification(title: String, body: String, fireAt: Date)
    }

    let id = UUID()
    let kind: Kind

    var title: String {
        switch kind {
        case .call: "Open phone dialer"
        case .message: "Prepare a message"
        case .notification: "Schedule a notification"
        }
    }

    var detail: String {
        switch kind {
        case .call(let number):
            return "The system will ask before placing a call to \(number)."
        case .message(let recipient, let body):
            return "Recipient: \(recipient)\n\n\(body)"
        case .notification(let title, let body, let fireAt):
            let when = fireAt.formatted(date: .abbreviated, time: .shortened)
            let extra = body.isEmpty ? "" : "\n\n\(body)"
            return "\(title)\n\nFires \(when).\(extra)"
        }
    }

    /// The confirmation button's label. Naming the action rather than saying "OK" is the
    /// difference between a decision and a dismissal.
    var confirmTitle: String {
        switch kind {
        case .call: "Open dialer"
        case .message: "Compose message"
        case .notification: "Schedule it"
        }
    }
}

@MainActor
final class SystemActionStore: ObservableObject {
    @Published var pending: PendingSystemAction?

    /// Outcome of the last confirmed action, so the person gets a receipt rather than silence.
    @Published var lastOutcome: String?

    func prepareCall(number: String) {
        pending = PendingSystemAction(kind: .call(number: number))
    }

    func prepareMessage(recipient: String, body: String) {
        pending = PendingSystemAction(kind: .message(recipient: recipient, body: body))
    }

    func prepareNotification(title: String, body: String, fireAt: Date) {
        pending = PendingSystemAction(kind: .notification(title: title, body: body, fireAt: fireAt))
    }

    func clear() {
        pending = nil
    }
}
