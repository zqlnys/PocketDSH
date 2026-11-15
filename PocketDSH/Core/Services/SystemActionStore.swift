import Combine
import Foundation

struct PendingSystemAction: Identifiable, Equatable {
    enum Kind: Equatable {
        case call(number: String)
        case message(recipient: String, body: String)
    }

    let id = UUID()
    let kind: Kind

    var title: String {
        switch kind {
        case .call: "Open phone dialer"
        case .message: "Prepare a message"
        }
    }

    var detail: String {
        switch kind {
        case .call(let number):
            return "The system will ask before placing a call to \(number)."
        case .message(let recipient, let body):
            return "Recipient: \(recipient)\n\n\(body)"
        }
    }
}

@MainActor
final class SystemActionStore: ObservableObject {
    @Published var pending: PendingSystemAction?

    func prepareCall(number: String) {
        pending = PendingSystemAction(kind: .call(number: number))
    }

    func prepareMessage(recipient: String, body: String) {
        pending = PendingSystemAction(kind: .message(recipient: recipient, body: body))
    }

    func clear() {
        pending = nil
    }
}
