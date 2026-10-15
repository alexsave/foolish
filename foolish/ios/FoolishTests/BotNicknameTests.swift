import XCTest
@testable import Foolish

/// A bot's stored name is shown as stored, minus the bot prefix - the web's
/// botDisplayName (src/common/botName.ts), rule for rule.
///
/// Found playing online: the roster is stored as cities ('%Vienna 1') since
/// 2026-09-22, and the phone's parser knew only the old ordnance names, so it
/// handed every live bot name back untouched and the seat read "%Vienna 1".
final class BotNicknameTests: XCTestCase {

    func testALiveBotNameLosesThePrefix() {
        XCTAssertEqual(BotNames.displayNickname("%Vienna 1"), "Vienna 1")
        XCTAssertEqual(BotNames.displayNickname("%St. Petersburg 7"), "St. Petersburg 7")
        XCTAssertEqual(BotNames.displayNickname("%New York 2"), "New York 2")
    }

    /// An old replay names the bot that played it, and the web shows that name:
    /// there is no display map to send it through any more.
    func testAnOldReplayNameIsShownAsStored() {
        XCTAssertEqual(BotNames.displayNickname("%Octogen 1"), "Octogen 1")
        XCTAssertEqual(BotNames.displayNickname("%Cordite Max 2"), "Cordite Max 2")
    }

    /// Only the leading prefix marks a bot; a person's name is never touched.
    func testAHumanNameIsUntouched() {
        for name in ["Sveta", "Владимир", "🤡", "", " %Vienna 1", "Vienna 1"] {
            XCTAssertEqual(BotNames.displayNickname(name), name)
        }
    }
}
