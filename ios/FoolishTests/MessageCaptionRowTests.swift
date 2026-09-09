// MessageCaptionRowTests — the row under the bubble picture
// (`MSMessageTemplateLayout.caption`), which used to be the literal string
// "Foolish" on every bubble of every game and now reports the table.
//
// Not the same row as `MessageCaptionBoundaryTests`, which despite its name
// pins `summaryText` — the backward-looking line that says what the sender did.
// This one is the forward-looking half: who the table is waiting on. The two
// come from ONE read of the bubble's bytes (MessageSummary's type doc), and the
// point of testing the caption over whole played games rather than over a
// hand-built state is that the property it must hold is a property of the
// KERNEL's board, not of any state a test could think to construct: whatever
// the kernel says the defender is, that is the seat the caption names, at every
// bubble of every bout.
//
// It also pins what the caption must NEVER carry. The bubble picture is the
// public table by construction (BubbleSnapshot renders viewer -1), and this row
// rides the same balloon onto the same lock screens — so it is held to the same
// rule by the same means: it is built from the spectator's view, and a card
// face appearing in it at all would be a leak.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageCaptionRowTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    private static let names = ["Alex", "Ann"]

    /// What the caption is entitled to call a seat: its join name, or the
    /// numbered fallback for a seat that never joined by name. Spelled out here
    /// because the sweep's later bubbles carry only the genesis creator's join,
    /// so hard-coding "Ann" for seat 1 would test the fixture and not the code.
    ///
    /// It is the naming rule and NOT the choice of seat, which is the part
    /// under test: every assertion below also checks that the caption is not
    /// the same sentence about somebody else.
    private func expected(_ key: String, seat: Int, env: MessageEnvelope) -> String {
        let name = env.joins.first { $0.seat == seat }?.name
            ?? FStrings.t("ios.msg.seatn", ["n": "\(seat + 1)"])
        return FStrings.t(key, ["name": name])
    }

    /// The composer's own caption, for the chain `payload` describes — through
    /// `publicRead`, which is the single read `MessagesViewController.stage`
    /// makes before it builds the picture, the caption and the summary.
    private func captionFor(payload: Data) async throws -> (text: String, view: GameView?,
                                                            env: MessageEnvelope) {
        let read = try await MessageKernel.shared.publicRead(payload: payload)
        return (MessageSummary.caption(env: read.env, view: read.view), read.view, read.env)
    }

    /// Every live bubble of twelve whole games names the seat the kernel calls
    /// the defender — and nobody else.
    ///
    /// The assertion is deliberately the WHOLE string and not "contains the
    /// name": a caption that named the defender by accident while also naming
    /// the attacker, the turn, or a card would pass a containment check, and
    /// the value of this row is that it says one thing.
    func testEveryLiveBubbleNamesTheDefender() async throws {
        var bubbles = 0
        var mismatches: [String] = []

        for salt in UInt8(1)...UInt8(12) {
            var payload: Data?
            var env: MessageEnvelope?

            for step in 0..<60 {
                let seat = step % 2
                let c: MessageTurnController
                if let p = payload, let e = env {
                    c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                } else {
                    c = MessageTurnController(genesisSeed: seed(salt), players: 2,
                                              gameId: 42,
                                              myNickname: Self.names[seat])
                }
                await c.begin()
                if c.view?.isOver == true { break }
                guard let move = c.legal.first(where: { $0.type != .wait }) else { continue }
                await c.apply(move)
                guard !c.pending.isEmpty else { continue }
                // A minute ago, so the round-16 pickup hold never gates the
                // sweep — this test is about the caption, not about timing.
                let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                let (text, view, e) = try await captionFor(payload: sealed)
                bubbles += 1

                let fool = view?.gameOver ?? -1
                if e.phase == 3 {
                    // The result card: the fool, by name. `line` announces the
                    // same seat in the summary; they read the fool off the same
                    // view, so a disagreement here is the two having drifted.
                    if fool >= 0 {
                        let want = expected("ios.msg.isfool", seat: fool, env: e)
                        if text != want {
                            mismatches.append("seed \(salt) step \(step) FINISHED: " +
                                              "\(text) != \(want)")
                        }
                    }
                } else if let d = view?.defender, d >= 0, fool < 0 {
                    let want = expected("ios.msg.cap.defends", seat: d, env: e)
                    if text != want {
                        mismatches.append("seed \(salt) step \(step) seat \(seat): " +
                                          "\(text) != \(want) (kernel defender \(d))")
                    }
                    // …and it is not the same sentence about anyone else. The
                    // seat is the whole content of this row; naming the wrong
                    // one reads as authoritative and is worse than saying
                    // nothing.
                    for other in 0..<2 where other != d {
                        if text == expected("ios.msg.cap.defends", seat: other, env: e) {
                            mismatches.append("seed \(salt) step \(step): caption names " +
                                              "seat \(other), kernel defender is \(d)")
                        }
                    }
                }
                if text.isEmpty {
                    mismatches.append("seed \(salt) step \(step): EMPTY caption")
                }
                payload = sealed
                env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
            }
        }

        XCTAssertGreaterThan(bubbles, 50, "the sweep did not actually play any games")
        XCTAssertEqual(mismatches, [], "captions that do not name the kernel's own defender")
    }

    /// The caption is names and words. No rank, no suit, no card — the same
    /// rule the picture keeps, and for the same reason: this row shows on a
    /// lock screen, to everyone in the thread.
    func testCaptionNeverCarriesACardFace() async throws {
        let suits = ["♠", "♥", "♦", "♣"]
        var leaks: [String] = []
        var bubbles = 0

        for salt in UInt8(20)...UInt8(25) {
            var payload: Data?
            var env: MessageEnvelope?
            for step in 0..<60 {
                let seat = step % 2
                let c: MessageTurnController
                if let p = payload, let e = env {
                    c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                } else {
                    c = MessageTurnController(genesisSeed: seed(salt), players: 2,
                                              gameId: 43,
                                              myNickname: Self.names[seat])
                }
                await c.begin()
                if c.view?.isOver == true { break }
                guard let move = c.legal.first(where: { $0.type != .wait }) else { continue }
                await c.apply(move)
                guard !c.pending.isEmpty else { continue }
                let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                let (text, _, _) = try await captionFor(payload: sealed)
                bubbles += 1
                if let s = suits.first(where: { text.contains($0) }) {
                    leaks.append("seed \(salt) step \(step): \(text) carries \(s)")
                }
                payload = sealed
                env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
            }
        }

        XCTAssertGreaterThan(bubbles, 20, "the sweep did not actually play any games")
        XCTAssertEqual(leaks, [], "a card face reached the bubble's caption row")
    }

    /// A WAITING lobby names the room and NOT the count. Round-5 M9, the owner:
    /// "no capacity text, too confusing" — the lobby's picture is its roster,
    /// and the caption must not turn into the "N of M" line that ruling threw
    /// out. Pinned by asserting the exact string, so adding a count breaks it.
    func testLobbyCaptionNamesTheRoomAndNotTheCount() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(9), players: 8)
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 910,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: [MessageJoin(seat: 0, name: "Alex"),
                                               MessageJoin(seat: 1, name: "Ann")])
        let (text, _, env) = try await captionFor(payload: payload)
        XCTAssertEqual(env.phase, 0, "WAITING")
        XCTAssertEqual(text, FStrings.t("ios.lobby"))
        XCTAssertFalse(text.contains("2"), "no capacity text on a lobby caption")
        XCTAssertFalse(text.contains("8"), "no capacity text on a lobby caption")
    }

    /// Bytes that would not parse still get a caption — the brand line. The
    /// extension stages such a bubble anyway (they are its own seal), and a
    /// balloon with an empty row under the picture is worse than one carrying
    /// the app's name.
    func testUnreadableBytesFallBackToTheBrand() {
        XCTAssertEqual(MessageSummary.caption(env: nil, view: nil), MessageSummary.brand)
    }
}
