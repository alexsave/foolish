// LiveArrivalFixture - a scripted N-seat chain of REAL bubbles, and a board
// controller standing on any one of them, for posing a live arrival headless.
//
// Every live-arrival suite before this one was two-player, and two players
// cannot pose the shape most of the 4p reports are about: a GOOD that is
// still pending when the next bubble lands. At two seats the one attacker's
// good over a covered table closes the bout on the spot, so a pending good
// never exists on the wire. At three or more it is the commonest state of a
// bout (docs/IMESSAGE_BODY_CODEC.md puts it at 47% of 4p mid-game cuts), and
// it is exactly the state the codec treats specially: a pending good is an
// atom only while nothing follows it (c/src/replay.c log_atom_kind), so a
// chain's `turn` can go DOWN from one bubble to its child.
//
// What it builds, all through MessageKernel and the real kernel:
//   - `coveredTable`: a deal played out at random until an N-seat table is in
//     the one window a good-run needs - every seat still in, every attack
//     covered, nobody good yet, stock left to refill from - sealed as the
//     ROOT bubble.
//   - `play`: one more bubble on top of any bubble, sealed the way a phone
//     seals it (MessageKernel.resealFromBase over the parent's bytes, parent8
//     = the first eight bytes of the parent's digest, the acting seat as the
//     last actor). So the delta (`n_new`) and the Rule P link are the real
//     ones, not a test's guess.
//   - `controller`: a MessageTurnController for one seat on one bubble, begun
//     (a cold open, floor -1) and optionally told a board is watching - which
//     is the state a live arrival lands on.
//   - `arrive`: hand a bubble to that controller the way GameSurface.seatOnBoard
//     does (`offerArrival`), and finish a retraction if one was started, the
//     way the board does when its red flight lands.
//
// FIDELITY. This is the controller half of a live arrival and nothing else:
// no MessagesViewController (isMine, present()'s markers, the stage
// generation, didStartSending), no GameSurface (Rule P refusal, the byte
// dedupe, the surface plan, the reload-is-arrival branch), no board (the
// view-change router, the role sync, the flights). `arrive` mirrors what
// GameSurface does AFTER Rule P has accepted the bubble; `preferred` is there
// for a test that wants to ask Rule P first, as the surface does. The harness
// scenario `arrival` (HarnessUI/HarnessScenario.swift) is the layer that runs
// the surface and the board.

import XCTest
@testable import FoolishKit

/// One bubble of a scripted chain: the bytes a phone would put in the thread,
/// the header those bytes decode to, and who did what to make it.
@MainActor
struct ChainBubble {
    let payload: Data
    let env: MessageEnvelope
    /// The seat that sealed it (-1 for the root, which is many moves at once).
    let actor: Int
    let move: Move?
    var turn: Int { env.turn }
    var digest8: Data { MessageTurnController.firstEight(hex: env.digest) }
}

@MainActor
struct LiveArrivalFixture {
    static let gameId: UInt64 = 0x4A11_4E

    let players: Int
    let joins: [MessageJoin]
    /// Every attack covered, nobody good yet, stock left.
    let root: ChainBubble
    let defender: Int
    /// The non-defender seats, in seat order. All of them may say good on the
    /// root, so the last of them to do so closes the bout.
    let attackers: [Int]

    private static var kernel: MessageKernel { MessageKernel.shared }

    /// A deal played to the window a good-run needs, as a sealed root bubble.
    ///
    /// `players` is any 3-8. `extra` narrows the search further (it is asked of
    /// a candidate root and may read the resident game, which IS that root at
    /// the time it is asked). Deterministic: the same arguments find the same
    /// root on every run, because the deal seeds and the playout's choices are
    /// both derived from the salt.
    static func coveredTable(players: Int, needDeck: Bool = true,
                             salts: ClosedRange<Int> = 1...200,
                             extra: ((LiveArrivalFixture) async throws -> Bool)? = nil)
        async throws -> LiveArrivalFixture {
        let joins = (0..<players).map { MessageJoin(seat: $0, name: "P\($0)") }
        for salt in salts {
            let seed = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ salt &* 7) | 1 })
            try await kernel.newGame(seed: seed, players: players)
            var rng = UInt64(salt) &* 0x9E37_79B9_7F4A_7C15 | 1
            func next(_ n: Int) -> Int {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17
                return Int(rng % UInt64(max(n, 1)))
            }
            var lastActor = 0
            for _ in 0..<400 {
                guard let v = await kernel.residentView(viewer: -1), v.gameStatus == .playing,
                      !v.isOver else { break }
                if await window(v, players: players, needDeck: needDeck) {
                    let attackers = v.players.map(\.seat).filter { $0 != v.defender }
                    let sealed = try await kernel.seal(phase: 2, lastActorSeat: lastActor,
                                                       gameId: gameId,
                                                       parent8: Data(repeating: 0, count: 8),
                                                       joins: joins)
                    let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
                    let f = LiveArrivalFixture(players: players, joins: joins,
                                               root: ChainBubble(payload: sealed, env: env,
                                                                 actor: -1, move: nil),
                                               defender: v.defender, attackers: attackers)
                    // `extra` may move the resident game; put the root back
                    // before the playout carries on from it.
                    if try await extra?(f) ?? true { return f }
                    _ = try await kernel.decode(payload: sealed, viewer: -1)
                }
                // One random legal move by a random seat. Goods are drawn
                // less often than they are offered, or most bouts would end
                // on the first good and the window would hardly ever open.
                let order = (0..<players).map { ($0 + next(players)) % players }
                var acted = false
                for s in order {
                    let legal = await kernel.residentLegal(seat: s).filter { $0.type != .wait }
                    guard !legal.isEmpty else { continue }
                    let plays = legal.filter { $0.type != .good }
                    let pick = (!plays.isEmpty && next(4) != 0) ? plays[next(plays.count)]
                                                                 : legal[next(legal.count)]
                    if (try? await kernel.apply(seat: s, move: pick)) != nil {
                        lastActor = s; acted = true; break
                    }
                }
                if !acted { break }
            }
        }
        throw XCTSkip("no \(players)p deal in \(salts.count) salts reached a fully covered table "
                      + "with every seat in and nobody good")
    }

    private static func window(_ v: GameView, players: Int, needDeck: Bool) async -> Bool {
        guard v.players.count == players, v.players.allSatisfy({ !$0.isOut }),
              v.defender >= 0, !v.battles.isEmpty,
              v.battles.allSatisfy({ $0.defense != nil }),
              v.goodMask == 0,
              !needDeck || v.deckCount > 0 else { return false }
        for s in v.players.map(\.seat) where s != v.defender {
            guard await kernel.residentLegal(seat: s).contains(where: { $0.type == .good })
            else { return false }
        }
        return true
    }

    /// One more bubble: `seat` plays `move` on `parent`, sealed the way a phone
    /// seals it. Throws if the kernel refuses the move.
    func play(_ seat: Int, _ move: Move, after parent: ChainBubble) async throws -> ChainBubble {
        let k = Self.kernel
        let sealed = try await k.resealFromBase(.continuation(payload: parent.payload),
                                                replaying: [move], seat: seat,
                                                gameId: Self.gameId,
                                                parent8: parent.digest8, joins: joins)
        let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
        return ChainBubble(payload: sealed, env: env, actor: seat, move: move)
    }

    /// `seat` says good on `parent`.
    func good(_ seat: Int, after parent: ChainBubble) async throws -> ChainBubble {
        try await play(seat, .good, after: parent)
    }

    /// The legal moves `seat` has on `bubble` (decodes it - the resident game
    /// is that bubble afterwards).
    func legal(_ seat: Int, on bubble: ChainBubble) async throws -> [Move] {
        _ = try await Self.kernel.decode(payload: bubble.payload, viewer: seat)
        return await Self.kernel.residentLegal(seat: seat)
    }

    /// The board `viewer` would be shown on `bubble`, from the kernel.
    func truth(_ viewer: Int, on bubble: ChainBubble) async throws -> GameView? {
        _ = try await Self.kernel.decode(payload: bubble.payload, viewer: viewer)
        return await Self.kernel.residentView(viewer: viewer)
    }

    /// Rule P, as GameSurface.maybeAdoptIncoming asks it: < 0 keeps `showing`,
    /// > 0 adopts `arriving`.
    func preferred(showing: ChainBubble, arriving: ChainBubble) async throws -> Int {
        try await Self.kernel.preferred(showing.payload, arriving.payload)
    }

    /// A board's controller for `seat`, opened on `bubble` (a cold open), with
    /// a board mounted on it when `watching` - which is what makes an arrival
    /// over a staged move a RETRACTION rather than a silent adopt.
    func controller(seat: Int, on bubble: ChainBubble, watching: Bool = true) async
        -> MessageTurnController {
        let c = MessageTurnController(parentPayload: bubble.payload, parent: bubble.env,
                                      mySeat: seat)
        await c.begin()
        if watching { c.setBoardWatching(true) }
        return c
    }

    /// Deliver `bubble` to `c` the way the surface does once Rule P has
    /// accepted it. A retraction (an arrival over a staged move) is finished
    /// at once when `finishRetraction` - the board's red flight landing - so
    /// the caller sees the adopted chain; pass false to look at the state
    /// BETWEEN the offer and the finish.
    func arrive(_ bubble: ChainBubble, at c: MessageTurnController,
                finishRetraction: Bool = true) async {
        await c.offerArrival(payload: bubble.payload, parent: bubble.env)
        if finishRetraction, c.conflictRetracting { await c.finishConflictAdopt() }
    }
}

extension Array where Element == GameEvent {
    /// The kinds a stream carries, in order - what an assertion message prints.
    var kindNames: String {
        map { $0.kind.map { "\($0)" } ?? "?\($0.type)" }.joined(separator: ",")
    }
    func has(_ kind: EventType) -> Bool { contains { $0.kind == kind } }
    /// Does this stream carry a bout end's SWEEP - the table flying off to the
    /// discard (`cardsToTrash`, what a closing good emits) or a `discard`?
    var sweepsTheTable: Bool { has(.cardsToTrash) || has(.discard) }
}
