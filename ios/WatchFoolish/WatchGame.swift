// WatchGame.swift — the watch's view-model over the REAL engine (Option G,
// docs/WATCHOS_G_SPEC.md). Wraps a FoolishKit `LocalGame` (offline vs the C bots)
// and derives exactly what the Table/Roster screens render: the seat strip, the
// battle pairs, the Crown focus list (hand + terminal), and — for each focused
// item — the single pill/tap outcome from §5. No Durak rule lives here; every
// legality answer is the kernel's legal menu.

import SwiftUI
import Combine

// MARK: - Seat / roster rows

/// One entry of the SeatStrip (§2) — counts only, no names.
struct SeatChip: Identifiable {
    let seat: Int
    let count: Int
    let isSelf: Bool
    let isDefender: Bool
    /// The seat that still OWES the opening attack — the strip's red. Transient: red says
    /// "the table is waiting on them", so it clears the moment they attack.
    let isOpening: Bool
    let isGood: Bool
    let isOut: Bool
    var id: Int { seat }
}

/// One RosterScreen row (§3).
struct RosterRow: Identifiable {
    let seat: Int
    let name: String
    let count: Int
    let isSelf: Bool
    let isDefender: Bool
    let isOpening: Bool
    let isGood: Bool
    let isOut: Bool
    var id: Int { seat }
}

// MARK: - Focus + pill model (§4, §5)

/// A Crown focus target: a hand card, or the single terminal item (GOOD ✓ / TAKE +n).
enum FocusItem: Equatable {
    case card(Card)
    case terminal
}

/// What the terminal item *is* for this role (§4). Attacker/bystander vote GOOD;
/// the defender takes the table. Both are bare icons — the count the mock printed next to
/// pickup is gone (owner review); the table list already shows what you'd be taking.
enum TerminalKind: Equatable {
    case good          // bare green ✓
    case pickup        // red ↓ — the cards come down into your hand
}

/// The verb under the fisheye (§4.6). H replaces G's coloured pill with a
/// complication-sized caption; per owner review it is uppercase and gray for every
/// verb — the lane, not the caption, carries the colour.
enum Verb: String {
    case attack    = "ATTACK"
    case cover     = "COVER"
    case coverPass = "COVER/PASS"
    case pass      = "PASS"
    case pickup    = "PICKUP"       // engine vocabulary for the defender's terminal
    case good      = "GOOD"
    case voted     = "VOTED"
}

/// One cover target inside the chooser (§5 G2b).
struct CoverOption: Identifiable { let attack: Card; let move: Move; var id: String { attack.identity } }
/// The pass choice. It carries no receiver name — the chooser shows a bare ↑ (owner
/// review); who catches it is the engine's business, and naming them cluttered the screen.
struct PassOption { let move: Move }

/// The modal presented for an ambiguous cover/pass (§5 G2b). In play this is always built
/// from the kernel's legal-move list (see `decision(for:)`) — never hand-assembled.
struct ChooserSpec: Identifiable {
    let id = UUID()
    let card: Card
    let coverTargets: [CoverOption]
    let pass: PassOption?

    /// A sample spec for inspecting the ChooserOverlay layout (`-chooser` flag).
    ///
    /// It must be a state the kernel could actually deal, so: you hold 9♥ with hearts
    /// trump, and the table's attacks are 9♠ and 9♣. The trump 9 beats both (cover), and
    /// because every attack shares rank 9 it may also be passed on (`game.c:731` — pass
    /// requires the table's attacks to share the passed value). A card that could cover
    /// *different* ranks could never also pass.
    static let demo = ChooserSpec(
        card: Card(s: 1, v: 9),
        coverTargets: [
            CoverOption(attack: Card(s: 0, v: 9), move: Move(type: .cover)),
            CoverOption(attack: Card(s: 2, v: 9), move: Move(type: .cover)),
        ],
        pass: PassOption(move: Move(type: .pass)))
}

/// The resolved caption + tap outcome for a focused item (§5, rendered per §4.6).
/// `action` is what a tap on the focused lane card (or the caption) commits — a move,
/// a chooser to present, or nothing.
struct ActionDecision {
    let verb: Verb
    let action: Action
    enum Action { case commit(Move); case chooser(ChooserSpec); case none }
    var caption: String { verb.rawValue }
}

@MainActor
final class WatchGame: ObservableObject {
    /// nil ONLY for a preview game (see `init(preview:)`), which has no kernel behind it.
    @Published private(set) var local: LocalGame!
    /// Bumped whenever the kernel rejects a commit — the Table flashes RejectGlow (§7).
    @Published private(set) var rejectPulse = 0
    /// A card thrown/covered optimistically, rendered translucent until the next
    /// authoritative snapshot replaces it (§7).
    @Published private(set) var optimistic: Card?

    let players: Int
    let botStrategy: Int
    private var bag = Set<AnyCancellable>()

    /// octogen — the top of the ladder (semtex + a stage-3 opponent-reply tournament) and
    /// the watch's opponent. It ships in the offline roster already (`ios_api.c` ROSTER,
    /// built with `-DCD_LEAFBOOK` so its endgame oracle is live); the watch simply never
    /// asked for it. The fallbacks matter only if a slimmer library is ever linked.
    ///
    /// It is a heavy MC solver, so a hot device still downgrades seats to espresso via
    /// LocalGame's thermal guard (§7.2) — that guard is the reason this is safe to ask for.
    static let defaultStrategy: Int = strategy(named: "octogen")

    /// Resolve a roster name to its id, falling back down the ladder.
    static func strategy(named want: String) -> Int {
        let roster = EngineC.roster()
        for name in [want, "cordite", "robusta", "firecracker"] {
            if let m = roster.first(where: { $0.name.lowercased() == name }) { return m.id }
        }
        return roster.last?.id ?? 0
    }

    /// The opponent's roster name, for display.
    var botName: String {
        EngineC.roster().first { $0.id == botStrategy }?.name ?? "bot"
    }

    /// The watch reads slower than the phone. Bots act asynchronously to you now (they no
    /// longer wait for your move), so at the server's 600–1200 ms an 8-seat table rewrites
    /// itself faster than you can follow it on a wrist — especially the one column you're
    /// actually reading. Tunable per surface; the phone keeps the server's pace.
    static let botPacing: ClosedRange<UInt64> = 1500...2600

    init(players: Int, botStrategy: Int) {
        self.players = players
        self.botStrategy = botStrategy
        self.staticView = nil
        self.staticLegal = []
        LocalGame.botPacingMS = WatchGame.botPacing
        local = WatchGame.make(players: players, strategy: botStrategy)
        subscribe()
    }

    // MARK: - Preview seam

    /// A game backed by a FIXED view and legal menu — no kernel, no bots, no LocalGame.
    /// This exists so `#Preview` can render any table state instantly and deterministically
    /// (see Previews.swift); booting a real game in a preview would be slow, random, and
    /// would fight the single-global C kernel. Nothing in the app uses this.
    init(preview view: GameView, legal: [Move] = []) {
        self.players = view.numPlayers
        self.botStrategy = -1
        self.staticView = view
        self.staticLegal = legal
        self.local = nil
    }

    /// Set for preview games only; when present it wins over the kernel's view.
    private let staticView: GameView?
    private let staticLegal: [Move]
    private var isPreview: Bool { staticView != nil }

    private static func make(players: Int, strategy: Int) -> LocalGame {
        var strategies: [Int: Int] = [:]
        for seat in 1..<max(players, 1) { strategies[seat] = strategy }
        return LocalGame(seed: seed(), players: players, humanSeat: 0, strategies: strategies)
    }
    private static func seed() -> Data { Data((0..<32).map { _ in UInt8.random(in: 0...255) }) }

    private func subscribe() {
        bag.removeAll()
        local.objectWillChange.sink { [weak self] _ in
            guard let self else { return }
            // A new authoritative snapshot supersedes any optimistic card.
            self.optimistic = nil
            // Surface a reject as a glow pulse (§7).
            if self.local.lastReject != nil { self.rejectPulse &+= 1 }
            self.objectWillChange.send()
        }.store(in: &bag)
    }

    func rematch() {
        local = WatchGame.make(players: players, strategy: botStrategy)
        optimistic = nil
        subscribe()
        objectWillChange.send()
    }

    // MARK: derived table state

    private var view: GameView? { staticView ?? local?.view }
    var deckCount: Int { view?.deckCount ?? 0 }
    var discardCount: Int { view?.discardCount ?? 0 }
    /// `-pairs` injects sample battles so the table list can be inspected without waiting
    /// for a live state; `-pairs7` overflows it (7 > 5 rows) to exercise the scroll+fade.
    static let demoBattles: [BattleView]? = {
        let args = ProcessInfo.processInfo.arguments
        let base: [BattleView] = [
            BattleView(attack: Card(s: 0, v: 12), defense: Card(s: 0, v: 13)),  // ♠K ← ♠A
            BattleView(attack: Card(s: 1, v: 7),  defense: Card(s: 1, v: 9)),   // ♥7 ← ♥9
            BattleView(attack: Card(s: 2, v: 6),  defense: nil),                // ♣6 open
        ]
        if args.contains("-pairs7") {
            return base + [
                BattleView(attack: Card(s: 3, v: 10), defense: Card(s: 3, v: 11)), // ♦Q ← ♦J
                BattleView(attack: Card(s: 0, v: 4),  defense: Card(s: 0, v: 8)),
                BattleView(attack: Card(s: 1, v: 11), defense: nil),
                BattleView(attack: Card(s: 2, v: 9),  defense: nil),
            ]
        }
        return args.contains("-pairs") ? base : nil
    }()
    var battles: [BattleView] { Self.demoBattles ?? view?.battles ?? [] }
    var hand: [Card] { view?.me?.hand ?? [] }
    var flipped: Card? { view?.flipped }
    var trumpSuit: Suit? { view?.trumpSuit }
    var isOver: Bool { view?.isOver ?? false }
    var viewer: Int { view?.viewer ?? 0 }
    var amDefender: Bool { view.map { $0.defender == $0.viewer } ?? false }
    var amOut: Bool { view?.me?.isOut ?? false }
    var iVoted: Bool { view.map { $0.hasSaidGood($0.viewer) } ?? false }
    /// Cards currently on the table (all attacks + covers) — TAKE's n.
    var tableCardCount: Int { battles.reduce(0) { $0 + 1 + ($1.defense == nil ? 0 : 1) } }

    var foolName: String? {
        guard let f = local?.foolSeat, let v = view else { return nil }
        return f == v.viewer ? "You" : (v.player(f)?.name ?? "Bot")
    }

    /// Whether the human has any legal action right now.
    var canAct: Bool { !legal.isEmpty && !isOver }

    /// Seat strip in seat order from your left, wrapping, you last (§2 SeatStrip).
    var seatStrip: [SeatChip] {
        guard let v = view else { return [] }
        let n = v.numPlayers
        return (1...n).map { off -> SeatChip in
            let s = (v.viewer + off) % n
            let p = v.player(s)
            return SeatChip(seat: s,
                            count: p?.handCount ?? 0,
                            isSelf: s == v.viewer,
                            isDefender: v.defender == s,
                            isOpening: v.firstAttacker == s && v.battles.isEmpty,
                            isGood: v.hasSaidGood(s),
                            isOut: p?.isOut ?? false)
        }
    }

    /// Offline bot seats carry no kernel name (only a "p{n}" id); give them stable,
    /// human handles so the roster and notifications read like a real table (§3, §9).
    private static let botNames = ["Kat", "Boris", "Mira", "Nils", "Zoya", "Oleg", "Petra"]
    private func name(for p: PlayerView) -> String {
        if p.seat == viewer { return "you" }
        if !p.name.isEmpty { return p.name }
        return Self.botNames[(p.seat - 1) % Self.botNames.count]
    }

    /// Roster rows: in-play by seat, eliminated pushed to the bottom (§3).
    var rosterRows: [RosterRow] {
        guard let v = view else { return [] }
        let rows = v.players.map { p in
            RosterRow(seat: p.seat, name: name(for: p),
                      count: p.handCount, isSelf: p.seat == v.viewer,
                      isDefender: v.defender == p.seat,
                      isOpening: v.firstAttacker == p.seat && v.battles.isEmpty,
                      isGood: v.hasSaidGood(p.seat), isOut: p.isOut)
        }
        return rows.sorted { a, b in
            if a.isOut != b.isOut { return !a.isOut }   // in-play first
            return a.seat < b.seat
        }
    }

    var outNames: [String] {
        guard let v = view else { return [] }
        return v.eliminationOrder.compactMap { seat in v.player(seat).map(name(for:)) }
    }

    // MARK: legality lookups (built once per snapshot, §4)

    private var legal: [Move] { isPreview ? staticLegal : (local?.humanLegal ?? []) }

    /// card → the single-card attack/throw-in move that plays it.
    private var attackMoves: [Card: Move] {
        var m: [Card: Move] = [:]
        for mv in legal where mv.type == .attack {
            for c in mv.cards where m[c] == nil { m[c] = Move(type: .attack, cards: [c]) }
        }
        return m
    }
    /// cover card → the attacks it may cover, each with the concrete cover move.
    private var coverMoves: [Card: [CoverOption]] {
        var m: [Card: [CoverOption]] = [:]
        for mv in legal where mv.type == .cover {
            guard let cover = mv.cards.first, let attack = mv.attackCards?.first else { continue }
            m[cover, default: []].append(CoverOption(attack: attack, move: mv))
        }
        return m
    }
    /// card → the pass (перевод) move that forwards defence with it.
    private var passMoves: [Card: Move] {
        var m: [Card: Move] = [:]
        for mv in legal where mv.type == .pass {
            for c in mv.cards where m[c] == nil { m[c] = mv }
        }
        return m
    }
    private var pickupMove: Move? { legal.first { $0.type == .pickup } }
    private var goodMove: Move? { legal.first { $0.type == .good } }

    // MARK: focus list (§4)

    /// The terminal item exists ONLY when it actually does something. The opener cannot say
    /// GOOD before attacking (`legal.c:377-384`), so during that window the lane is just
    /// your hand — showing a ✓ there offers an option the kernel would refuse.
    var hasTerminal: Bool { decision(for: .terminal) != nil }

    /// hand cards (server order) + the terminal if it's live, unless you're out/over.
    var focusCount: Int { (amOut || isOver) ? 0 : hand.count + (hasTerminal ? 1 : 0) }
    func item(at index: Int) -> FocusItem {
        index < hand.count ? .card(hand[index]) : .terminal
    }
    var terminalKind: TerminalKind { amDefender ? .pickup : .good }

    /// Auto-focus target on activation: first legal card, else the terminal (§4).
    var firstLegalIndex: Int {
        for (i, c) in hand.enumerated() where decision(for: .card(c)) != nil { return i }
        return hasTerminal ? hand.count : 0
    }

    /// Whether a hand card is playable right now (dims it in the fisheye lane, §4.6).
    func isLegal(_ card: Card) -> Bool { decision(for: .card(card)) != nil }

    // MARK: the §5 decision — one outcome per focused item

    func decision(for item: FocusItem) -> ActionDecision? {
        switch item {
        case .terminal:
            if amDefender {
                guard tableCardCount > 0, pickupMove != nil else { return nil }
                return ActionDecision(verb: .pickup, action: .commit(.pickup))
            }
            if let g = goodMove {
                return ActionDecision(verb: .good, action: .commit(g))
            }
            if iVoted {
                return ActionDecision(verb: .voted, action: .none)
            }
            return nil

        case .card(let c):
            if let a = attackMoves[c] {
                return ActionDecision(verb: .attack, action: .commit(a))
            }
            if amDefender {
                let covers = coverMoves[c] ?? []
                let pass = passMoves[c]
                if covers.count == 1 && pass == nil {
                    return ActionDecision(verb: .cover, action: .commit(covers[0].move))
                }
                // Ambiguous cover (≥2 targets) or the cover-or-pass fork: the caption
                // names the fork, the tap opens the chooser.
                if covers.count >= 2 || (covers.count >= 1 && pass != nil) {
                    let spec = ChooserSpec(card: c, coverTargets: covers,
                                           pass: pass.map { PassOption(move: $0) })
                    return ActionDecision(verb: pass != nil ? .coverPass : .cover, action: .chooser(spec))
                }
                if covers.isEmpty, let p = pass {
                    return ActionDecision(verb: .pass, action: .commit(p))
                }
            }
            return nil
        }
    }

    // MARK: intents (§7 optimistic commit)

    func commit(_ move: Move) {
        guard !isPreview else { return }        // a preview table is a still life
        // Optimistically show the played card translucent until the next snapshot.
        if move.type == .attack || move.type == .cover || move.type == .pass {
            optimistic = move.cards.first
        }
        local.play(move)
        objectWillChange.send()
    }
}
