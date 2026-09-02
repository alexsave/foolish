// ROUND 30 — the two fine-tuning nits from 1.0(30), both about a value that is
// read off the board a beat too late.
//
//  1. "when the cards fly out of the hand to cover some cards on the table, the
//     card count in the players hand doesn't update until the cards LAND. So if
//     you were to screenshot it, it would look like a player that previously
//     had 6 cards has 6 cards in hand + 2 currently flying. If two are
//     currently flying, it shoud display 4 cards in hand, right? ... And vice
//     versa, if we have some move that puts cards INTO a players hand, the card
//     count shouldn't update until the cards GET TO THE HAND."
//
//  2. "for the sword -> check rotation, the width doesn't QUITE go to zero
//     during rotate out before swapping to the other glyph and rotating in."
import XCTest
@testable import FoolishKit

@MainActor
final class Round30NitTests: XCTestCase {

    // MARK: 1 — which direction a count moves on, and when

    /// The rule as a table, so "leaving drops early, arriving ticks late" is
    /// asserted rather than inferred from where a line sits in a loop.
    func testOnlyCardsLEAVINGAHandDropTheBadgeEarly() {
        for kind in [EventType.attackPass, .cover] {
            XCTAssertTrue(MessageTableView.badgeDropsAsCardsLeave(kind),
                          "\(kind) takes cards OUT of a hand - the badge drops as they lift")
        }
        for kind in [EventType.pickup, .refill, .deal] {
            XCTAssertFalse(MessageTableView.badgeDropsAsCardsLeave(kind),
                           "\(kind) puts cards INTO a hand - the badge waits for them to land")
        }
        for kind in [EventType.discard, .cardsToTrash, .magicTransition, .out] {
            XCTAssertFalse(MessageTableView.badgeDropsAsCardsLeave(kind),
                           "\(kind) moves nobody's hand")
        }
    }

    // MARK: 2 — the coin is edge-on before the face changes

    func testTheCollapseFinishesBeforeTheSwap() {
        XCTAssertLessThan(roleFlipCollapse, roleFlipHalf,
                          "the collapse has to be DONE when the face is swapped, not still running")
        XCTAssertEqual(roleFlipCollapse + roleFlipSettle, roleFlipHalf, accuracy: 1e-9,
                       "and the settle is taken OUT of the collapse - the gesture's total "
                       + "length is tuned against the card flights it captions and must not move")
    }

    func testTheSettleIsLongerThanTheFrameItExistsToCover() {
        // The bug is a one-frame head start: the timer runs from `withAnimation`
        // and the animation runs from the next display refresh. Anything less
        // than a 60Hz frame does not cover it.
        XCTAssertGreaterThan(roleFlipSettle, 1.0 / 60.0,
                             "a settle shorter than one frame does not fix a one-frame skew")
    }

    func testTheSettleCannotSwallowTheCollapse() {
        // Proportional at the low end, so a short flip (or a future shorter
        // flightTime) cannot leave the collapse with nothing - or negative time.
        XCTAssertGreaterThan(roleFlipCollapse, 0)
        XCTAssertLessThanOrEqual(roleFlipSettle, roleFlipHalf * 0.3 + 1e-9,
                                 "never more than a third of the half-turn")
    }
}
