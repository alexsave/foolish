// Round4Tests — the round-4 board/lobby notes that reduce to a pure decision,
// pinned so they cannot quietly come back.
//
// Notes 3 (the replayed turn) and 5 (the double pickup / count twitch) are
// pinned where they actually live: MessageEventsTests drives the kernel's turn
// grouping end to end, and the veil that fixes the first-paint half is a
// SwiftUI @State handoff whose visible consequence is the cover tilt
// (CoverTiltTests). What is left here is note 1 and note 6, both of which are
// one function each.
import XCTest
@testable import FoolishKit

final class Round4Tests: XCTestCase {

    // MARK: note 1 — "if you were the last one to send an invite, shouldn't
    // have the Send invite pop up."

    /// The reported state: I created the lobby, so the newest bubble on this
    /// chain is my own invite — already sent, or sitting auto-staged in the
    /// compose field. Offering "Send invite" there asks for a second copy of
    /// it, which is what the creator hit every single time.
    func testMyOwnInviteIsNotOfferedBackToMe() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 1, capacity: 8,
                                             iSentTheInvite: true),
                       .waiting)
    }

    /// …but the recovery path survives for a lobby whose newest bubble is
    /// someone ELSE's: there the invite I could send is genuinely not in the
    /// thread yet.
    func testAnInviteIsStillOfferedWhenTheNewestBubbleIsNotMine() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 1, capacity: 8,
                                             iSentTheInvite: false),
                       .invite)
    }

    /// Authorship must not reach the UNSEATED branches — a full lobby is still
    /// full and a joinable one still joinable no matter who sent the newest
    /// bubble. This test used to also pin "two players still means Start" for
    /// BOTH authorship values; round-5 M9 deliberately broke that half (the
    /// newest bubble's sender is withheld from Start while the lobby has
    /// room), so the seated case now asserts the M9 split instead —
    /// Round5LobbyTests owns the full enumeration of that gate.
    func testAuthorshipOnlyAffectsTheSeatedBranches() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 2, capacity: 8,
                                             iSentTheInvite: false), .start)
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 2, capacity: 8,
                                             iSentTheInvite: true), .waiting,
                       "round-5 M9: the newest bubble's sender cannot also start a lobby with room")
        for mine in [false, true] {
            XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 1, capacity: 8,
                                                 iSentTheInvite: mine), .join)
            XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 8, capacity: 8,
                                                 iSentTheInvite: mine), .full)
        }
    }

    /// Every (mySeat, joined, capacity, authorship) has exactly one answer and
    /// none of them is "nothing at all" — the enum exists precisely because the
    /// old if/else chain had an invisible state with no control in it.
    func testEveryLobbyShapeResolvesToSomething() {
        for capacity in [2, 8] {
            for joined in 0...capacity {
                for seat in [nil, 0] as [Int?] {
                    for mine in [false, true] {
                        _ = LobbyControls.offered(mySeat: seat, joined: joined,
                                                  capacity: capacity, iSentTheInvite: mine)
                    }
                }
            }
        }
    }

    // MARK: note 6 — "position of flipped card should be constant relative to
    // the top left corner throughout the game. Same with the bottom card."

    /// Both anchors are constants, not functions of the count. Written as a
    /// value check rather than a rendering check because the defect was
    /// literally a `deckCount > 0 ? … : …` in the offset: the flipped card
    /// hopped 10pt left and 20pt up on the draw that emptied the stock.
    func testTheFlippedCardAndBottomCardAnchorsAreConstants() {
        XCTAssertEqual(FDeckWell.flippedOrigin.x, FSpace.s + 10, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.flippedOrigin.y, FSpace.s + 20, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.bottomCardOrigin.x, FSpace.s, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.bottomCardOrigin.y, FSpace.s, accuracy: 0.001)
    }

    /// The flipped card sits centred under the stock's LANDSCAPE footprint —
    /// which is what makes the constant honest rather than arbitrary: it is the
    /// position the card already had while the stock was there, kept after it
    /// empties instead of dropping back to the bare inset.
    func testTheFlippedCardStaysCentredUnderWhereTheStockWas() {
        let stockVisualWidth: CGFloat = 66   // a 46x66 card laid landscape
        let cardW: CGFloat = 46
        XCTAssertEqual(FDeckWell.flippedOrigin.x - FSpace.s,
                       (stockVisualWidth - cardW) / 2, accuracy: 0.001)
    }

    // MARK: "attackers shouldn't even have the option to say good until all
    // cards are covered" — as a SET, for the callers that ask "can this seat
    // do anything at all" rather than "is this one button live".

    private func battle(_ attack: Card, _ defense: Card?) -> BattleView {
        BattleView(attack: attack, defense: defense)
    }

    /// The kernel always offers GOOD; the board must not, while anything on
    /// the table is still uncovered. An attacker with nothing to throw in is
    /// then left with NO move — which is correct, and is the state that froze
    /// an 8-player auto-run whose turn handoff was reading the raw menu.
    func testGoodIsWithheldUntilTheTableIsFullyCovered() {
        let uncovered = [battle(Card(s: 0, v: 10), nil)]
        XCTAssertTrue(CardPlay.humanMoves(battles: uncovered,
                                          legal: [Move(type: .good, cards: [])]).isEmpty,
                      "an attacker cannot say good over an uncovered attack")

        let covered = [battle(Card(s: 0, v: 10), Card(s: 0, v: 12))]
        XCTAssertEqual(CardPlay.humanMoves(battles: covered,
                                           legal: [Move(type: .good, cards: [])]).count, 1,
                       "…and can once every attack is covered")
    }

    /// `wait` is never a move, and an empty table is not "fully covered" — a
    /// bout nobody has opened has nothing to say good about.
    func testWaitIsNeverOfferedAndAnEmptyTableIsNotCovered() {
        XCTAssertTrue(CardPlay.humanMoves(battles: [], legal: [Move(type: .wait, cards: [])]).isEmpty)
        XCTAssertTrue(CardPlay.humanMoves(battles: [], legal: [Move(type: .good, cards: [])]).isEmpty)
    }

    /// Every other move passes through untouched — the gate is about `good`
    /// alone and must not quietly filter a cover or a pickup.
    func testEveryOtherMoveSurvivesTheGate() {
        let uncovered = [battle(Card(s: 0, v: 10), nil)]
        let legal = [Move(type: .good, cards: []), Move(type: .wait, cards: []),
                     Move(type: .pickup, cards: []), Move(type: .cover, cards: [Card(s: 0, v: 12)])]
        let out = CardPlay.humanMoves(battles: uncovered, legal: legal)
        XCTAssertEqual(Set(out.map(\.type)), [.pickup, .cover])
    }
}
