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
/// the defender takes the table.
enum TerminalKind: Equatable {
    case good          // bare green ✓
    case take(Int)     // red +n
}

/// The colour pair of a pill (§2 Pill Color Palette): foreground on background.
enum PillStyle {
    case attack, cover, pass, take, good, voted
    var fg: Color {
        switch self {
        case .attack, .cover: return WColor.gold
        case .pass:  return WColor.blue
        case .take:  return WColor.red
        case .good:  return WColor.green
        case .voted: return Color(hex: 0x2C2C2E)
        }
    }
    var bg: Color {
        switch self {
        case .attack, .cover: return Color(hex: 0x241A02)
        case .pass:  return Color(hex: 0x001B38)
        case .take:  return Color(hex: 0x2B0300)
        case .good:  return Color(hex: 0x03210E)
        case .voted: return WColor.seat
        }
    }
}

/// One cover target inside the chooser (§5 G2b).
struct CoverOption: Identifiable { let attack: Card; let move: Move; var id: String { attack.identity } }
struct PassOption { let receiver: String; let move: Move }

/// The modal presented for an ambiguous cover/pass (§5 G2b).
struct ChooserSpec: Identifiable {
    let id = UUID()
    let card: Card
    let title: String
    let coverTargets: [CoverOption]
    let pass: PassOption?

    /// A sample spec for inspecting the ChooserOverlay layout (`-chooser` flag).
    static let demo = ChooserSpec(
        card: Card(s: 0, v: 9),
        title: "9♠ — cover or pass?",
        coverTargets: [
            CoverOption(attack: Card(s: 0, v: 7), move: Move(type: .cover)),
            CoverOption(attack: Card(s: 0, v: 8), move: Move(type: .cover)),
        ],
        pass: PassOption(receiver: "Mira", move: Move(type: .pass)))
}

/// The resolved pill + tap outcome for a focused item (§5). `action` is what a tap on
/// the FocusSlot/Pill commits — a move, a chooser to present, or nothing.
struct PillDecision {
    let label: String
    let style: PillStyle
    let action: Action
    enum Action { case commit(Move); case chooser(ChooserSpec); case none }
}

@MainActor
final class WatchGame: ObservableObject {
    @Published private(set) var local: LocalGame
    /// Bumped whenever the kernel rejects a commit — the Table flashes RejectGlow (§7).
    @Published private(set) var rejectPulse = 0
    /// A card thrown/covered optimistically, rendered translucent until the next
    /// authoritative snapshot replaces it (§7).
    @Published private(set) var optimistic: Card?

    let players: Int
    let botStrategy: Int
    private var bag = Set<AnyCancellable>()

    /// robusta is foolish's default human opponent (strong, no ML, cheap on-watch).
    static let defaultStrategy: Int = {
        let roster = EngineC.roster()
        for want in ["robusta", "cordite", "firecracker"] {
            if let m = roster.first(where: { $0.name.lowercased().contains(want) }) { return m.id }
        }
        return roster.last?.id ?? 0
    }()

    init(players: Int, botStrategy: Int) {
        self.players = players
        self.botStrategy = botStrategy
        local = WatchGame.make(players: players, strategy: botStrategy)
        subscribe()
    }

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

    private var view: GameView? { local.view }
    var deckCount: Int { view?.deckCount ?? 0 }
    var discardCount: Int { view?.discardCount ?? 0 }
    /// `-pairs` injects sample battles (a covered pair, another cover, an open attack)
    /// so the TablePager layout can be inspected without waiting for a live state.
    static let demoBattles: [BattleView]? =
        ProcessInfo.processInfo.arguments.contains("-pairs") ? [
            BattleView(attack: Card(s: 0, v: 12), defense: Card(s: 0, v: 13)),  // ♠K ← ♠A
            BattleView(attack: Card(s: 1, v: 7),  defense: Card(s: 1, v: 9)),   // ♥7 ← ♥9
            BattleView(attack: Card(s: 2, v: 6),  defense: nil),               // ♣6 open
        ] : nil
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
        guard let f = local.foolSeat, let v = view else { return nil }
        return f == v.viewer ? "You" : (v.player(f)?.name ?? "Bot")
    }

    /// Whether the human has any legal action right now.
    var canAct: Bool { !local.humanLegal.isEmpty && !isOver }

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
                      isDefender: v.defender == p.seat, isOut: p.isOut)
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

    private var legal: [Move] { local.humanLegal }

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

    /// The receiver a pass would hand the defence to: next in-play seat clockwise.
    private var passReceiverName: String {
        guard let v = view else { return "next" }
        let n = v.numPlayers
        for off in 1...n {
            let s = (v.defender + off) % n
            if s == v.defender { break }
            if let p = v.player(s), !p.isOut { return p.seat == v.viewer ? "you" : p.name }
        }
        return "next"
    }

    // MARK: focus list (§4)

    /// hand cards (server order) + one terminal item, unless you're out/over.
    var focusCount: Int { (amOut || isOver) ? 0 : hand.count + 1 }
    func item(at index: Int) -> FocusItem {
        index < hand.count ? .card(hand[index]) : .terminal
    }
    var terminalKind: TerminalKind { amDefender ? .take(tableCardCount) : .good }

    /// Auto-focus target on activation: first legal card, else the terminal (§4).
    var firstLegalIndex: Int {
        for (i, c) in hand.enumerated() where decision(for: .card(c)) != nil { return i }
        return hand.count   // terminal
    }

    /// Whether a hand card is playable right now (drives chip dimming, §2 ChipStrip).
    func isLegal(_ card: Card) -> Bool { decision(for: .card(card)) != nil }

    // MARK: the §5 decision — one outcome per focused item

    func decision(for item: FocusItem) -> PillDecision? {
        switch item {
        case .terminal:
            if amDefender {
                guard tableCardCount > 0, pickupMove != nil else { return nil }
                return PillDecision(label: "TAKE \(tableCardCount)", style: .take,
                                    action: .commit(.pickup))
            }
            if let g = goodMove {
                return PillDecision(label: "GOOD", style: .good, action: .commit(g))
            }
            if iVoted {
                return PillDecision(label: "✓ voted", style: .voted, action: .none)
            }
            return nil

        case .card(let c):
            if let a = attackMoves[c] {
                return PillDecision(label: "ATTACK", style: .attack, action: .commit(a))
            }
            if amDefender {
                let covers = coverMoves[c] ?? []
                let pass = passMoves[c]
                if covers.count == 1 && pass == nil {
                    return PillDecision(label: "COVER", style: .cover, action: .commit(covers[0].move))
                }
                if covers.count >= 2 || (covers.count >= 1 && pass != nil) {
                    let title = pass != nil ? "\(CardRank.label(c.v))\(c.suit?.glyph ?? "") — cover or pass?"
                                            : "\(CardRank.label(c.v))\(c.suit?.glyph ?? "") covers which?"
                    let spec = ChooserSpec(card: c, title: title, coverTargets: covers,
                                           pass: pass.map { PassOption(receiver: passReceiverName, move: $0) })
                    return PillDecision(label: "COVER", style: .cover, action: .chooser(spec))
                }
                if covers.isEmpty, let p = pass {
                    return PillDecision(label: "PASS ▸ \(passReceiverName)", style: .pass, action: .commit(p))
                }
            }
            return nil
        }
    }

    // MARK: intents (§7 optimistic commit)

    func commit(_ move: Move) {
        // Optimistically show the played card translucent until the next snapshot.
        if move.type == .attack || move.type == .cover || move.type == .pass {
            optimistic = move.cards.first
        }
        local.play(move)
        objectWillChange.send()
    }
}
