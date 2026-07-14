// WatchGame.swift — the watch's view-model over the REAL engine. Wraps a
// FoolishKit `LocalGame` (offline vs the C bots) and exposes exactly what the
// watch screens render, all derived from the kernel's `GameView` / legal menu.
// No Durak rule lives here (§3, WATCHOS_APP_PLAN §6) — the engine owns them,
// including cover/pass/perevod, so the pills are always the kernel's truth.

import SwiftUI
import Combine

/// A ring seat, resolved from the masked view.
struct Seat: Identifiable {
    let seat: Int
    let name: String
    let count: Int
    let isSelf: Bool
    let isDefender: Bool
    let isAttacker: Bool
    let isOut: Bool
    var id: Int { seat }
}

/// A bottom action button = one entry of the kernel's legal menu.
struct Pill: Identifiable {
    let id = UUID()
    let label: String
    let move: Move
    var isPrimary: Bool { move.type != .pass }
}

@MainActor
final class WatchGame: ObservableObject {
    @Published private(set) var local: LocalGame
    @Published var selected: Card?

    let players: Int
    let botStrategy: Int
    private var bag = Set<AnyCancellable>()

    /// The bot to seat the opponents with — resolved once from the kernel roster.
    /// robusta is foolish's default human opponent (strong, no ML, cheap on-watch);
    /// fall back through cordite, then whatever the last roster entry is.
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
    private static func seed() -> Data {
        Data((0..<32).map { _ in UInt8.random(in: 0...255) })
    }
    private func subscribe() {
        bag.removeAll()
        local.objectWillChange.sink { [weak self] _ in
            guard let self else { return }
            // Drop a selection that's no longer in hand after a move.
            if let s = self.selected, !(self.local.view?.me?.hand?.contains(s) ?? false) { self.selected = nil }
            self.objectWillChange.send()
        }.store(in: &bag)
    }

    func rematch() {
        local = WatchGame.make(players: players, strategy: botStrategy)
        selected = nil
        subscribe()
        objectWillChange.send()
    }

    // MARK: derived state

    private var view: GameView? { local.view }
    var deckCount: Int { view?.deckCount ?? 0 }
    var discardCount: Int { view?.discardCount ?? 0 }
    var battles: [BattleView] { view?.battles ?? [] }
    var hand: [Card] { view?.me?.hand ?? [] }
    var trump: Card? { view?.flipped }
    var trumpSuit: Suit? { view?.trumpSuit }
    /// Whether it's your move (you have a legal menu) — drives the brass state.
    var yourTurn: Bool { !local.humanLegal.isEmpty && !(view?.isOver ?? false) }

    /// A short status for the nav-bar title line beside the clock — the one
    /// glanceable fact the ring doesn't already show: is it on you to act.
    var turnText: String {
        if view?.isOver ?? false { return "Game over" }
        return yourTurn ? "Your move" : "Waiting…"
    }

    var foolName: String? {
        guard let f = local.foolSeat, let v = view else { return nil }
        return f == v.viewer ? "You" : (v.player(f)?.name ?? "Bot")
    }

    /// You at index 0 (6 o'clock), then the opponents by seat.
    var seats: [Seat] {
        guard let v = view else { return [] }
        var out: [Seat] = []
        if let me = v.me {
            out.append(Seat(seat: me.seat, name: "You", count: me.handCount, isSelf: true,
                            isDefender: v.defender == me.seat, isAttacker: v.firstAttacker == me.seat, isOut: me.isOut))
        }
        for p in v.players where p.seat != v.viewer {
            out.append(Seat(seat: p.seat, name: p.name, count: p.handCount, isSelf: false,
                            isDefender: v.defender == p.seat, isAttacker: v.firstAttacker == p.seat, isOut: p.isOut))
        }
        return out
    }

    /// The kernel's legal menu for the current selection: a selected card offers
    /// the moves that use it (attack / cover / pass), nothing selected offers the
    /// card-free moves (pickup / good). Deduped by type (one pill each).
    var pills: [Pill] {
        let all = local.humanLegal
        let moves: [Move] = selected.map { c in all.filter { $0.cards.contains(c) } }
            ?? all.filter { $0.type == .pickup || $0.type == .good }
        var seen = Set<MoveType>()
        return moves.compactMap { m in
            guard !seen.contains(m.type), let l = Self.label(m.type) else { return nil }
            seen.insert(m.type)
            return Pill(label: l, move: m)
        }
    }
    private static func label(_ t: MoveType) -> String? {
        switch t {
        case .attack: return "Attack"
        case .cover:  return "Cover"
        case .pass:   return "Pass"
        case .pickup: return "Pickup"
        case .good:   return "Good"
        default:      return nil
        }
    }

    // MARK: intents

    func toggle(_ card: Card) { selected = (selected == card) ? nil : card }
    func play(_ move: Move) { local.play(move); selected = nil }
}
