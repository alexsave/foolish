// note 11 (HARNESS_NOTES_R2): `conversation.insert`-ing a staged bubble makes
// it `selectedMessage`; the auto-collapse that follows re-presents with THAT
// selection. Without StagedBubbleRouting, that reads as a brand-new incoming
// bubble and tears down + rebuilds the live board, which then replays the
// move it just watched itself play (the cache commits on send, not insert).
// Pure logic, no MSConversation/MSMessage needed.
import XCTest
@testable import FoolishKit

final class StagedBubbleRoutingTests: XCTestCase {

    private func url(_ payload: Data) -> URL { MessageEnvelope.link(payload: payload) }

    /// The exact note-11 sequence: my own staged bubble becomes selected.
    /// Must reuse `lastPayloadURL` (the URL the live board is already keyed
    /// on) rather than route the staged bubble's own URL, which would look
    /// like a fresh selection and rebuild the board.
    func testOwnStagedBubbleReusesTheLastPayloadURL() {
        let openedWith = Data([1, 2, 3, 4])
        let justStaged = Data([9, 9, 9, 9, 9])   // my move, sealed into a new chain
        let resolved = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: url(justStaged), startingNewGame: false,
            pendingStage: (payload: justStaged, mySeat: 0),
            lastPayloadURL: url(openedWith))
        XCTAssertEqual(resolved, url(openedWith),
                       "must keep presenting the URL the live board is already on")
        XCTAssertNotEqual(resolved, url(justStaged),
                          "must NOT route the staged bubble's own URL back in as if new")
    }

    /// A genuinely different incoming bubble (an opponent's real reply, or a
    /// human tap on some other bubble) must still route normally — the fix
    /// must not swallow every selection while a stage is pending.
    func testADifferentIncomingBubbleIsNotSwallowed() {
        let staged = Data([9, 9, 9, 9, 9])
        let opponentsReply = Data([7, 7, 7])
        let resolved = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: url(opponentsReply), startingNewGame: false,
            pendingStage: (payload: staged, mySeat: 0),
            lastPayloadURL: url(Data([1, 2, 3])))
        XCTAssertEqual(resolved, url(opponentsReply), "a genuinely new bubble must still route")
    }

    /// No stage pending: the selection always passes through untouched,
    /// pending or not — this is the ordinary (non-note-11) path.
    func testNoPendingStagePassesSelectionThrough() {
        let selected = Data([4, 4, 4])
        let resolved = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: url(selected), startingNewGame: false,
            pendingStage: nil, lastPayloadURL: nil)
        XCTAssertEqual(resolved, url(selected))
    }

    /// New game always wins, regardless of what's selected or pending — the
    /// existing (pre-note-11) contract, unaffected by this fix.
    func testStartingNewGameAlwaysResolvesToNil() {
        let staged = Data([9, 9, 9])
        let resolved = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: url(staged), startingNewGame: true,
            pendingStage: (payload: staged, mySeat: 0), lastPayloadURL: url(staged))
        XCTAssertNil(resolved)
    }

    /// No selection at all (extension opened with nothing tapped): nil in,
    /// nil out, pending or not.
    func testNoSelectionResolvesToNil() {
        let resolved = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: nil, startingNewGame: false,
            pendingStage: (payload: Data([1]), mySeat: 0), lastPayloadURL: url(Data([2])))
        XCTAssertNil(resolved)
    }
    /// …and the same must hold AFTER the send. `didStartSending` clears
    /// `pendingStage` (that is the commit), but Messages leaves my bubble
    /// selected — so the next present() saw a "new" selection, reloaded the
    /// surface from my own just-sent chain, and the open-replay played the move
    /// I had just watched myself make. The second half of round-3's double
    /// animation, and the reason `lastSentPayload` exists.
    func testMyOwnJustSENTBubbleAlsoKeepsTheBoard() throws {
        let mine = Data([9, 8, 7, 6, 5])
        let url = MessageEnvelope.link(payload: mine)
        let previous = URL(string: "https://foolish.cards/m/1PREVIOUS")!

        // pendingStage is nil now (the send committed it) — lastSentPayload is
        // what recognises the bubble as mine.
        XCTAssertEqual(
            StagedBubbleRouting.resolvedPayloadURL(selectedURL: url, startingNewGame: false,
                                                   pendingStage: nil, lastPayloadURL: previous,
                                                   lastSentPayload: mine),
            previous, "my own sent bubble must not reload the board")

        // Someone else's bubble still routes normally.
        let theirs = MessageEnvelope.link(payload: Data([1, 2, 3, 4]))
        XCTAssertEqual(
            StagedBubbleRouting.resolvedPayloadURL(selectedURL: theirs, startingNewGame: false,
                                                   pendingStage: nil, lastPayloadURL: previous,
                                                   lastSentPayload: mine),
            theirs, "a bubble I did not send is a real new selection")
    }

    /// ROUND 12 #11 (owner: "sometimes animation replays when the bubble is
    /// sent - I saw this for an attack"). An ARRIVAL can be mine too: the
    /// simulator loops a sent bubble back to its sender, and a second device on
    /// the same iCloud account does it for real. Threaded on as new, the surface
    /// adopts it and arms the open-replay for MY OWN last move - filmed at
    /// 30fps, the card the player just played vanishes from the table the
    /// instant Send lands, and the attack animates again when the arming runs.
    ///
    /// `isMine` is what `didReceive` asks before threading anything on. It must
    /// recognise BOTH windows - staged-but-unsent, and already-sent - and must
    /// not swallow a genuine arrival, which would freeze the game.
    func testMyOwnChainIsRecognisedAsMineInBothWindows() {
        let staged = Data([9, 8, 7])
        let sent = Data([4, 4, 4, 4])
        let theirs = Data([1, 2, 3])

        XCTAssertTrue(StagedBubbleRouting.isMine(staged, pendingStage: staged,
                                                 lastSentPayload: nil),
                      "the bubble still in the input field is mine")
        XCTAssertTrue(StagedBubbleRouting.isMine(sent, pendingStage: nil,
                                                 lastSentPayload: sent),
                      "the bubble I already sent is mine")
        XCTAssertFalse(StagedBubbleRouting.isMine(theirs, pendingStage: staged,
                                                  lastSentPayload: sent),
                       "an opponent's move must still arrive, or the game freezes")
        XCTAssertFalse(StagedBubbleRouting.isMine(nil, pendingStage: staged,
                                                  lastSentPayload: sent),
                       "an undecodable arrival is nobody's chain, least of all mine")
        XCTAssertFalse(StagedBubbleRouting.isMine(theirs, pendingStage: nil,
                                                  lastSentPayload: nil),
                       "with nothing staged or sent, nothing is mine")
    }
}
