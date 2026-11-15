import XCTest
@testable import PocketDSHCore

final class SmokeTests: XCTestCase {
    func testVersion() {
        XCTAssertEqual(PocketDSHCore.version, "0.1.0")
    }
}
