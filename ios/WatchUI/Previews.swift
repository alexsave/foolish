// Previews.swift — the layout, in Xcode, on static data.
//
// This is the tuning loop: open this file (or HTuning.swift) in Xcode, edit a number, and
// the canvas redraws. Nothing here runs the C kernel, deals a game, or drives a bot — every
// screen below is a fixed `GameView` plus a fixed legal menu, so a preview is instant,
// deterministic, and shows the exact state you asked for instead of whatever a random deal
// happened to produce.
//
// It only supplies DATA. The layout is untouched: these previews render the same
// TableScreen / RosterScreen / ChooserOverlay the app ships.
//
// Editing the board:  `Deal` below is a small builder — change a hand, a battle, a count,
// the trump, who defends, who's escaped, and re-run. The hand string is "6h Ac 10s"; suits
// are s/h/c/d and ranks 2…10, J, Q, K, A.

import SwiftUI

// MARK: - Fixture builder

/// A static table. Everything the screens read comes from here, spelled out — no kernel.
enum Deal {

    /// "Qs" → ♠Q. Suits: s h c d (matching the engine's 0…3). Ranks: 2…10 J Q K A (A = 13).
    static func card(_ s: String) -> Card {
        let suits: [Character: Int] = ["s": 0, "h": 1, "c": 2, "d": 3]
        let suit = suits[s.last!] ?? 0
        let rank = String(s.dropLast())
        let v: Int
        switch rank.uppercased() {
        case "A": v = 13
        case "K": v = 12
        case "Q": v = 11
        default:  v = Int(rank) ?? 6
        }
        return Card(s: suit, v: v)
    }
    static func hand(_ s: String) -> [Card] { s.split(separator: " ").map { card(String($0)) } }

    /// One seat. `count` is what the strip shows; `hand` only matters for the viewer.
    static func player(_ seat: Int, _ name: String, _ count: Int,
                       hand: [Card]? = nil, out: Bool = false) -> PlayerView {
        PlayerView(seat: seat, name: name, status: out ? 1 : 0, handCount: count,
                   awaitingAttack: false, strategyKey: 0, hand: hand)
    }

    /// Assemble a view. `good` is the list of seats that have voted; `out` the escape order.
    static func view(players: [PlayerView], defender: Int, viewer: Int = 0,
                     firstAttacker: Int = 1, trump: Int = 1, deck: Int = 11, discard: Int = 0,
                     flipped: Card? = nil, battles: [BattleView] = [],
                     good: [Int] = [], out: [Int] = [], gameOver: Int = -1) -> GameView {
        GameView(status: 1, numPlayers: players.count, powerSuit: trump,
                 deckCount: deck, discardCount: discard, hasFlipped: flipped != nil,
                 firstAttacker: firstAttacker, defender: defender, viewer: viewer,
                 goodMask: good.reduce(0) { $0 | (1 << $1) }, gameOver: gameOver,
                 flipped: flipped, battles: battles, eliminationOrder: out, players: players)
    }

    static func battle(_ attack: String, _ defense: String? = nil) -> BattleView {
        BattleView(attack: card(attack), defense: defense.map(card))
    }

    // MARK: The 8-player table

    /// Eight seats: you (seat 0, 6 cards), six bots, one escaped. Trump ♥, the flip still
    /// under the deck. Tweak freely — this is the board every preview below starts from.
    static func eight(viewerHand: String = "6h Ah Kh 6s 10c Jd",
                      defender: Int = 3,
                      firstAttacker: Int = 2,
                      battles: [BattleView] = [],
                      good: [Int] = [],
                      deck: Int = 11, discard: Int = 0) -> GameView {
        view(players: [
                player(0, "you",   hand(viewerHand).count, hand: hand(viewerHand)),
                player(1, "Kat",   4),
                player(2, "Boris", 6),
                player(3, "Mira",  5),
                player(4, "Nils",  3),
                player(5, "Zoya",  6),
                player(6, "Oleg",  2),
                player(7, "Petra", 0, out: true),
             ],
             defender: defender, firstAttacker: firstAttacker, trump: 1,
             deck: deck, discard: discard,
             flipped: card("6h"), battles: battles, good: good, out: [7])
    }

    // MARK: Legal menus (what the caption reads is driven by these, as in play)

    /// Every hand card is attackable — the opener's menu.
    static func attackAll(_ h: [Card]) -> [Move] {
        h.map { Move(type: .attack, cards: [$0]) } + [Move(type: .good)]
    }
}

// MARK: - Table

#Preview("8p · attacker") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3,
        battles: [Deal.battle("Qs", "Ks"), Deal.battle("Kc")],
        good: [4, 6]),
        legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
        onOpenRoster: {})
}

#Preview("8p · nothing legal") {
    // No legal menu ⇒ every lane card dims and the caption is blank. This is the state
    // that used to render as mud before Glyph stopped fading dim cards wholesale.
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3,
        battles: [Deal.battle("Qs", "Ks"), Deal.battle("Kc")])),
        onOpenRoster: {})
}

#Preview("8p · defending, pickup") {
    // You defend, so the lane's terminal is the red ↓ and its caption reads PICKUP.
    TableScreen(game: WatchGame(preview: Deal.eight(
        viewerHand: "Ac Ks 7s 6h 10c",
        defender: 0,
        battles: [Deal.battle("8c", "Jc"), Deal.battle("Kc"), Deal.battle("Kd")]),
        legal: [Move(type: .pickup)]),
        onOpenRoster: {})
}

#Preview("8p · empty table") {
    TableScreen(game: WatchGame(preview: Deal.eight(defender: 3),
                                legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
                onOpenRoster: {})
}

#Preview("8p · 7 pairs, scrolls") {
    // Past `HTuning.tableVisibleRows` the list scrolls with edge fades.
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3,
        battles: [Deal.battle("6s", "9s"), Deal.battle("Jc", "Qc"), Deal.battle("Jd", "Ad"),
                  Deal.battle("Kc"), Deal.battle("9c"), Deal.battle("7h", "10h"),
                  Deal.battle("8d")],
        deck: 0, discard: 10)),
        onOpenRoster: {})
}

#Preview("8p · 11 cards") {
    // A post-pickup hand: same ±2 window, just more crown travel.
    let h = "3d 9h 9d 8d 2c Qs Jd 7c 6h Ah 4c"
    return TableScreen(game: WatchGame(preview: Deal.eight(viewerHand: h, defender: 3,
                                                           battles: [Deal.battle("9c")]),
                                       legal: Deal.attackAll(Deal.hand(h))),
                       onOpenRoster: {})
}

#Preview("8p · all voted GOOD") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3,
        battles: [Deal.battle("Qs", "Ks"), Deal.battle("Kc", "Ac")],
        good: [1, 2, 4, 5, 6]),
        legal: [Move(type: .good)]),
        onOpenRoster: {})
}

#Preview("2p · heads-up") {
    TableScreen(game: WatchGame(preview: Deal.view(
        players: [Deal.player(0, "you", 6, hand: Deal.hand("6h Ah Kh 6s 10c Jd")),
                  Deal.player(1, "Kat", 5)],
        defender: 1, deck: 23, flipped: Deal.card("6s"),
        battles: [Deal.battle("Qh")]),
        legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
        onOpenRoster: {})
}

#Preview("8p · every seat colour") {
    // One of each: red opener (seat 2 — red only shows while the table is EMPTY, i.e. the
    // bout is waiting on them), orange defender (3), green voted (4, 6), dark gray escaped
    // (7), white the rest — and YOU solid (seat 0, white).
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3, firstAttacker: 2,
        good: [4, 6]),
        legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
        onOpenRoster: {})
}

// MARK: - Chooser

#Preview("Chooser · cover or pass") {
    TableScreen(game: WatchGame(preview: Deal.eight(defender: 0,
                                                    battles: [Deal.battle("9s"), Deal.battle("9c")])),
                onOpenRoster: {})
        .overlay { ChooserOverlay(spec: .demo, onCover: { _ in }, onPass: { _ in }, onClose: {}) }
}

#Preview("Chooser · cover only") {
    let spec = ChooserSpec(card: Deal.card("Ah"),
                           coverTargets: [CoverOption(attack: Deal.card("9s"), move: Move(type: .cover)),
                                          CoverOption(attack: Deal.card("Kd"), move: Move(type: .cover))],
                           pass: nil)
    return TableScreen(game: WatchGame(preview: Deal.eight(defender: 0)), onOpenRoster: {})
        .overlay { ChooserOverlay(spec: spec, onCover: { _ in }, onPass: { _ in }, onClose: {}) }
}

// MARK: - Roster / list / over

#Preview("Roster") {
    RosterScreen(game: WatchGame(preview: Deal.eight(defender: 3, discard: 6)))
}

#Preview("Roster · escapees") {
    // Escaped players are SHOWN, not stated: dark gray, bottom of the list, no footer.
    RosterScreen(game: WatchGame(preview: Deal.view(
        players: [Deal.player(0, "you", 5, hand: Deal.hand("6h Ah Kh 6s 10c")),
                  Deal.player(1, "Kat", 4),
                  Deal.player(2, "Boris", 0, out: true),
                  Deal.player(3, "Mira", 6),
                  Deal.player(4, "Nils", 0, out: true),
                  Deal.player(5, "Zoya", 3),
                  Deal.player(6, "Oleg", 0, out: true),
                  Deal.player(7, "Petra", 0, out: true)],
        defender: 3, deck: 0, discard: 22, out: [2, 6, 4, 7])))
}

#Preview("Games list") { NavigationStack { GamesListView(onNew: { _ in }) } }

#Preview("Game over") {
    GameOverScreen(foolName: "Boris", escapeOrder: ["Kat", "Mira", "you", "Nils"]) {}
}
