// MoveWire.swift — decode the kernel's packed legal-move wire into [Move], in
// Swift, no JSON. The twin of the web's residentLegalMoves reader (engine.ts).
//
// Layout (c/wasm/wasm_api.c wasm_export_moves, also fio_legal_packed):
//   u32 count (LE), then per move:
//     type(1), n_cards(1), cards[n_cards](card byte), attacks[n_cards](card byte)
// The attack bytes are meaningful only for COVER; other moves ignore them.
// Card byte: suit*13 + (value-1); 0xFE/0xFF are never real move cards.

import Foundation
import CFoolish

public enum MoveWire {
    // Index → MoveType, matching MOVE_* in c/src/legal.h and MOVE_TYPE in the web.
    private static let types: [MoveType] = [.attack, .cover, .pass, .pickup, .good, .wait]

    /// A move's MOVE_* index, for the kernel entries that take a move TYPE
    /// rather than a whole move (`TurnWire.admit`). -1 for `.unknown`, which
    /// has no kernel number. Read off the table above rather than written out
    /// again, so there is one statement of this numbering in Swift.
    public static func wireIndex(_ type: MoveType) -> Int { types.firstIndex(of: type) ?? -1 }

    private static func card(_ b: UInt8) -> Card {
        if b >= 0xFE { return Card.hidden }
        let v = Int(b); return Card(s: v / 13, v: (v % 13) + 1)
    }

    /// A move as the awire action frame - the bytes `fio_apply_awire` takes,
    /// and the body an online move POSTs.
    ///
    /// THE KERNEL WRITES IT. This hands over the move's MOVE_* index and its
    /// cards as suit/value pairs and gets the frame back (`fio_awire_encode` ->
    /// c/src/awire.c), so the frame's shape - which kinds carry cards, how many
    /// a cover owes, what a card byte is - is stated in C and nowhere in Swift.
    /// It is the same reason `encode` below exists rather than a second menu
    /// writer: one format, one author.
    ///
    /// Swift wrote these bytes itself until the kernel grew a door, and the two
    /// copies that did (here and FoolishNet's PackedAction) had already drifted:
    /// this one trapped instead of refusing on a move of more than 255 cards,
    /// and wrote a cover with fewer attack cards than cover cards - a frame the
    /// decoder rejects - without noticing. The kernel refuses both, so those are
    /// now empty rather than wrong.
    ///
    /// Empty for anything the kernel will not write: `wait` and `unknown`, which
    /// have no action on the wire, and a structurally impossible move.
    public static func encodeAction(_ move: Move) -> [UInt8] {
        let cards = pairs(move.cards)
        // Only a cover pairs cover cards with the attacks they land on; every
        // other kind's frame has no room for them, so they are not offered.
        let attacks = move.type == .cover ? pairs(move.attackCards ?? []) : pairs([])
        var cap = 64
        while true {
            var out = [CChar](repeating: 0, count: cap)
            let n: Int32 = cards.withUnsafeBufferPointer { c in
                attacks.withUnsafeBufferPointer { a in
                    fio_awire_encode(Int32(wireIndex(move.type)),
                                     c.baseAddress, Int32(move.cards.count),
                                     a.baseAddress, Int32(move.type == .cover ? (move.attackCards ?? []).count : 0),
                                     &out, Int32(cap))
                }
            }
            if n >= 0 { return out.prefix(Int(n)).map { UInt8(bitPattern: $0) } }
            guard n == -3, cap < 4096 else { return [] }   // FIO_ECAP
            cap *= 2
        }
    }

    /// Cards as the kernel's suit/value PAIRS - two signed bytes each, so the
    /// card-byte arithmetic stays on the C side with the rest of the frame.
    /// `clamping` rather than a plain conversion because a nonsense card must
    /// come back as a refusal from the kernel, never as a trap here.
    ///
    /// Two zero bytes of tail so an EMPTY list still has a non-nil baseAddress;
    /// the kernel is told the count separately and never reads them.
    private static func pairs(_ cards: [Card]) -> [Int8] {
        var out: [Int8] = []
        out.reserveCapacity(cards.count * 2 + 2)
        for c in cards { out.append(Int8(clamping: c.s)); out.append(Int8(clamping: c.v)) }
        out.append(0); out.append(0)
        return out
    }

    /// THE MENU, WRITTEN. The twin of `decode` below, so a caller holding
    /// decoded moves can hand them back to the kernel to be asked a question
    /// about (`PlayWire`, whose rules take the menu as bytes). Production never
    /// needs it - every board is handed the kernel's own bytes and passes those
    /// straight on - so its user is the test that builds a menu by hand, which
    /// is exactly the thing that must not grow a second copy of this layout.
    ///
    /// A cover naming fewer attack cards than cover cards pads with the
    /// no-card sentinel, which reads back as `Card.hidden` and can never equal
    /// a real attack. `.unknown` moves cannot be written and are dropped.
    public static func encode(_ moves: [Move]) -> Data {
        func byte(_ c: Card) -> UInt8 { c.isHidden ? 0xFE : UInt8(c.s * 13 + (c.v - 1)) }
        // Filtered BEFORE the header is written, so the count can never
        // promise an entry the loop then declines to write.
        let writable = moves.compactMap { m in types.firstIndex(of: m.type).map { ($0, m) } }
        var out: [UInt8] = []
        let n = UInt32(writable.count)
        out.append(contentsOf: [UInt8(n & 0xFF), UInt8((n >> 8) & 0xFF),
                                UInt8((n >> 16) & 0xFF), UInt8((n >> 24) & 0xFF)])
        for (t, m) in writable {
            out.append(UInt8(t))
            out.append(UInt8(m.cards.count))
            out.append(contentsOf: m.cards.map(byte))
            let attacks = m.attackCards ?? []
            for i in 0..<m.cards.count { out.append(i < attacks.count ? byte(attacks[i]) : 0xFE) }
        }
        return Data(out)
    }

    /// A menu with no moves on it - the four-byte header alone.
    public static let emptyMenu = Data([0, 0, 0, 0])

    public static func decode(_ data: Data) -> [Move] {
        let b = [UInt8](data)
        guard b.count >= 4 else { return [] }
        var q = 4
        let n = Int(b[0]) | (Int(b[1]) << 8) | (Int(b[2]) << 16) | (Int(b[3]) << 24)
        var moves: [Move] = []
        moves.reserveCapacity(max(0, n))
        for _ in 0..<max(0, n) {
            guard q + 2 <= b.count else { break }
            let t = Int(b[q]); q += 1
            let k = Int(b[q]); q += 1
            guard q + 2 * k <= b.count else { break }
            let type = (t >= 0 && t < types.count) ? types[t] : .unknown
            var cards: [Card] = []; cards.reserveCapacity(k)
            for _ in 0..<k { cards.append(card(b[q])); q += 1 }
            var attacks: [Card] = []; attacks.reserveCapacity(k)
            for _ in 0..<k { attacks.append(card(b[q])); q += 1 }
            switch type {
            case .pickup, .good, .wait: moves.append(Move(type: type))
            case .cover:                moves.append(Move(type: .cover, cards: cards, attackCards: attacks))
            default:                    moves.append(Move(type: type, cards: cards))
            }
        }
        return moves
    }
}
