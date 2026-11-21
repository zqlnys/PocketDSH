import XCTest
@testable import PocketDSH

/// The rule the pending-action protocol exists to enforce: nothing reaches the system on the
/// model's word alone, and a person can always tell what they approved.
@MainActor
final class SystemActionTests: XCTestCase {

    func testNotificationIsStagedNotScheduled() {
        let store = SystemActionStore()
        XCTAssertNil(store.pending)

        let fireAt = Date().addingTimeInterval(600)
        store.prepareNotification(title: "Tea", body: "Steep", fireAt: fireAt)

        guard case .notification(let title, let body, let when)? = store.pending?.kind else {
            return XCTFail("expected a staged notification")
        }
        XCTAssertEqual(title, "Tea")
        XCTAssertEqual(body, "Steep")
        XCTAssertEqual(when, fireAt, "the staged action must carry the requested time unchanged")
    }

    /// The sheet's button has to name the action. "OK" is a dismissal; "Schedule it" is a
    /// decision, and only one of them tells the person what they are agreeing to.
    func testConfirmationNamesTheAction() {
        let store = SystemActionStore()
        store.prepareNotification(title: "t", body: "", fireAt: Date().addingTimeInterval(60))
        XCTAssertEqual(store.pending?.confirmTitle, "Schedule it")

        store.prepareCall(number: "123")
        XCTAssertEqual(store.pending?.confirmTitle, "Open dialer")

        store.prepareMessage(recipient: "a", body: "b")
        XCTAssertEqual(store.pending?.confirmTitle, "Compose message")
    }

    /// The detail text is the whole basis for the decision, so it has to carry the specifics
    /// rather than a generic description.
    func testDetailCarriesTheSpecifics() {
        let store = SystemActionStore()
        store.prepareNotification(title: "Water the plants", body: "Kitchen too",
                                  fireAt: Date().addingTimeInterval(3600))
        let detail = store.pending?.detail ?? ""
        XCTAssertTrue(detail.contains("Water the plants"), detail)
        XCTAssertTrue(detail.contains("Kitchen too"), detail)

        store.prepareCall(number: "+15550100")
        XCTAssertTrue(store.pending?.detail.contains("+15550100") == true)
    }

    func testClearDropsTheStagedAction() {
        let store = SystemActionStore()
        store.prepareCall(number: "1")
        XCTAssertNotNil(store.pending)
        store.clear()
        XCTAssertNil(store.pending)
    }
}
