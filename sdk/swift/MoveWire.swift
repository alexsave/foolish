// MoveWire.swift — decode the kernel's packed legal-move wire into [Move], in
// Swift, no JSON. The twin of the web's residentLegalMoves reader (engine.ts).
//
// Layout (c/wasm/wasm_api.c wasm_export_moves, also fio_legal_packed):
//   u32 count (LE), then per move:
//     type(1), n_cards(1), cards[n_cards](card byte), attacks[n_cards](card byte)
// The attack bytes are meaningful only for COVER; other moves ignore them.
// Card byte: suit*13 + (value-1); 0xFE/0xFF are never real move cards.

import Foundation

public enum MoveWire {
    // Index → MoveType, matching MOVE_* in c/src/legal.h and MOVE_TYPE in the web.
    private static let types: [MoveType] = [.attack, .cover, .pass, .pickup, .good, .wait]

    private static func card(_ b: UInt8) -> Card {
        if b >= 0xFE { return Card.hidden }
        let v = Int(b); return Card(s: v / 13, v: (v % 13) + 1)
    }

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
