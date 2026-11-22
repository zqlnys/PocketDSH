import XCTest
@testable import PocketDSH

/// Every confirmed system action has to leave a result behind.
///
/// The confirmation protocol exists so a person can approve something before it happens; it is
/// only half a protocol if the other half - saying what actually happened - is missing. Three of
/// the four paths were silent about failure: the dialer ignored its completion, the message
/// composer reported nothing, and only the notification path set an outcome at all.
final class SystemActionOutcomeTests: XCTestCase {

    @MainActor
    func testEveryKindHasAConfirmTitleThatNamesIt() {
        let store = SystemActionStore()
        let kinds: [PendingSystemAction.Kind] = [
            .call(number: "+15551234567"),
            .message(recipient: "+15551234567", body: "hello"),
            .notification(title: "Stand up", body: "stretch", fireAt: Date(timeIntervalSince1970: 1))
        ]
        for kind in kinds {
            let action = PendingSystemAction(kind: kind)
            XCTAssertFalse(action.confirmTitle.isEmpty, "\(kind) has no confirm title")
        }
        // The store starts with nothing pending and nothing claimed.
        XCTAssertNil(store.pending)
        XCTAssertNil(store.lastOutcome)
    }

    /// The outcome survives `clear()`, which is what makes it readable after the sheet closes -
    /// and the sheet closes before the person could otherwise see anything.
    @MainActor
    func testOutcomeSurvivesClearingThePendingAction() {
        let store = SystemActionStore()
        store.prepareCall(number: "+15551234567")
        XCTAssertNotNil(store.pending)
        store.lastOutcome = "This device has no dialer, so nothing was opened."
        store.clear()
        XCTAssertNil(store.pending, "clearing should drop the pending action")
        XCTAssertEqual(store.lastOutcome, "This device has no dialer, so nothing was opened.")
    }
}
