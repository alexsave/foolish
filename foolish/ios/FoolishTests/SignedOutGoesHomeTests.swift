import XCTest
@testable import Foolish

/// Signing out (or deleting the account) leaves the Dashboard.
///
/// Found on the simulator: after delete-account the app stayed on a Dashboard
/// with no name, whose Create game did nothing, because nothing told the
/// coordinator the session had ended.
@MainActor
final class SignedOutGoesHomeTests: XCTestCase {
    func testTheDashboardGivesWayToHomeWhenTheSessionEnds() {
        let c = AppCoordinator()
        c.screen = .dashboard
        c.sessionEnded()
        XCTAssertEqual(c.screen, .home)
    }

    func testAnOfflineTableIsNotInterrupted() {
        let c = AppCoordinator()
        c.screen = .table
        c.sessionEnded()
        XCTAssertEqual(c.screen, .table)
    }
}
