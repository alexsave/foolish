// MaskedView.swift — decode the kernel's packed masked-view wire straight into a
// GameView, in Swift, with NO JSON round-trip through the kernel.
//
// The wire is view.c `state_put`: a fixed byte layout the server produces and
// every client reads. This is the Swift twin of the web's TS reader — client and
// server are kernel-to-kernel, so the bytes ARE the contract; re-serialising them
// to JSON just to hand them to Codable was pure ceremony (owner decision: wipe
// the JSON). Decoding a wire is marshalling, not a game rule (§3) — the rules
// (legal moves) still come from the kernel over the same packed wire.
//
// Layout (see c/src/view.c state_put):
//   status, num_players, power_suit, first_attacker, defender,
//   discard_len(u16 LE), has_flipped, flipped(card), good_mask(u32 LE),
//   has_good_ts, deck_count(u16 LE), deck[deck_count](card),
//   num_battles, battles[n]{attack(card), defense(card)},
//   players[num_players]{status, awaiting, hand_count, hand[hand_count](card)},
//   num_eliminated, elimination[num_eliminated](seat)
//
// A card byte: 0..51 = suit*13 + (value-1); 0xFE = hidden; 0xFF = none.

import Foundation

public enum MaskedView {
    static let WIRE_HIDDEN: UInt8 = 0xFE
    static let WIRE_NONE: UInt8 = 0xFF

    /// A visible card, or nil for none (0xFF); hidden (0xFE) → Card.hidden.
    private static func card(_ b: UInt8) -> Card? {
        if b == WIRE_NONE { return nil }
        if b == WIRE_HIDDEN { return Card.hidden }
        let v = Int(b)
        return Card(s: v / 13, v: (v % 13) + 1)
    }

    /// Decode the packed masked state for `viewer` into a GameView. Player names
    /// are NOT on the wire (identity lives in the roster); callers merge them.
    /// Returns nil on a short/malformed buffer.
    public static func decode(_ bytes: Data, viewer: Int) -> GameView? {
        let b = [UInt8](bytes)
        var p = 0
        func u8() -> Int? { guard p < b.count else { return nil }; defer { p += 1 }; return Int(b[p]) }
        func u16() -> Int? { guard p + 1 < b.count else { return nil }; defer { p += 2 }; return Int(b[p]) | (Int(b[p+1]) << 8) }
        func u32() -> Int? {
            guard p + 3 < b.count else { return nil }; defer { p += 4 }
            return Int(b[p]) | (Int(b[p+1]) << 8) | (Int(b[p+2]) << 16) | (Int(b[p+3]) << 24)
        }
        func cardByte() -> UInt8? { guard p < b.count else { return nil }; defer { p += 1 }; return b[p] }

        guard let status = u8(), let numPlayers = u8(), let powerSuit = u8(),
              let firstAttacker = u8(), let defender = u8(),
              let discardCount = u16(), let hasFlippedRaw = u8(), let flippedByte = cardByte(),
              let goodMask = u32(), let _hasGoodTs = u8(), let deckCount = u16()
        else { return nil }
        _ = _hasGoodTs
        let hasFlipped = hasFlippedRaw != 0
        let flipped = hasFlipped ? card(flippedByte) ?? nil : nil

        // Deck cards (hidden in a masked view) — consumed, not surfaced by count.
        for _ in 0..<deckCount { guard cardByte() != nil else { return nil } }

        guard let numBattles = u8() else { return nil }
        var battles: [BattleView] = []
        battles.reserveCapacity(numBattles)
        for _ in 0..<numBattles {
            guard let ab = cardByte(), let db = cardByte(), let attack = card(ab) else { return nil }
            battles.append(BattleView(attack: attack, defense: card(db)))
        }

        var players: [PlayerView] = []
        players.reserveCapacity(numPlayers)
        for seat in 0..<numPlayers {
            guard let pstatus = u8(), let awaitingRaw = u8(), let handCount = u8() else { return nil }
            var hand: [Card]? = nil
            var visibleHand: [Card] = []
            var anyVisible = false
            for _ in 0..<handCount {
                guard let hb = cardByte() else { return nil }
                if hb == WIRE_HIDDEN { visibleHand.append(Card.hidden) }
                else if let c = card(hb) { visibleHand.append(c); anyVisible = true }
            }
            // The viewer's own hand is real cards; others are counts only (nil).
            if seat == viewer && anyVisible { hand = visibleHand }
            players.append(PlayerView(seat: seat, name: "", status: pstatus, handCount: handCount,
                                      awaitingAttack: awaitingRaw != 0, strategyKey: 0, hand: hand))
        }

        guard let numElim = u8() else { return nil }
        var elimination: [Int] = []
        for _ in 0..<numElim { guard let s = u8() else { return nil }; elimination.append(s) }

        // gameOver = the kernel's game_done rule read off the wire's statuses:
        // exactly one player still IN with everyone else OUT is the fool. This is
        // terminal state, not a move-legality decision (the web derives it the
        // same way in WinScreen).
        let statusIn = 2, statusOut = 3
        let inSeats = players.filter { $0.status == statusIn }
        let outCount = players.filter { $0.status == statusOut }.count
        let gameOver = (inSeats.count == 1 && outCount == numPlayers - 1) ? inSeats[0].seat : -1

        return GameView(
            status: status, numPlayers: numPlayers, powerSuit: powerSuit,
            deckCount: deckCount, discardCount: discardCount, hasFlipped: hasFlipped,
            firstAttacker: firstAttacker, defender: defender, viewer: viewer,
            goodMask: goodMask, gameOver: gameOver, flipped: flipped,
            battles: battles, eliminationOrder: elimination, players: players)
    }
}
