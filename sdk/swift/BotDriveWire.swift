// BotDriveWire.swift — decode the kernel's packed bot-drive result, no JSON.
//
// Layout (c/ios/ios_api.c fio_bot_drive_packed):
//   u32 n_actions (LE)
//   per action: seat(1), pace(1), type(1), n_cards(1), cards[n_cards], attacks[n_cards]
//   i32 stop (LE), i32 ended (LE), i32 delayMs (LE)
// Events are not carried (unused until B4 animation) — the decoded BotDrive has
// an empty events list.

import Foundation

public enum BotDriveWire {
    private static let types: [MoveType] = [.attack, .cover, .pass, .pickup, .good, .wait]
    private static func card(_ b: UInt8) -> Card {
        if b >= 0xFE { return Card.hidden }
        let v = Int(b); return Card(s: v / 13, v: (v % 13) + 1)
    }

    public static func decode(_ data: Data) -> BotDrive {
        let b = [UInt8](data)
        func empty() -> BotDrive { BotDrive(actions: [], events: [], stopRaw: 0, ended: -1, delayMs: 0) }
        guard b.count >= 4 else { return empty() }
        var q = 4
        let n = Int(b[0]) | (Int(b[1]) << 8) | (Int(b[2]) << 16) | (Int(b[3]) << 24)
        var actions: [BotAction] = []
        for _ in 0..<max(0, n) {
            guard q + 4 <= b.count else { return empty() }
            let seat = Int(b[q]); let pace = Int(b[q+1]); let t = Int(b[q+2]); let k = Int(b[q+3]); q += 4
            guard q + 2 * k <= b.count else { return empty() }
            var cards: [Card] = []; for _ in 0..<k { cards.append(card(b[q])); q += 1 }
            var attacks: [Card] = []; for _ in 0..<k { attacks.append(card(b[q])); q += 1 }
            let type = (t >= 0 && t < types.count) ? types[t] : .unknown
            actions.append(BotAction(seat: seat, type: type, cards: cards,
                                     attackCards: type == .cover ? attacks : nil, pace: pace))
        }
        func i32() -> Int {
            guard q + 4 <= b.count else { return 0 }
            let v = Int(Int32(bitPattern: UInt32(b[q]) | (UInt32(b[q+1]) << 8) | (UInt32(b[q+2]) << 16) | (UInt32(b[q+3]) << 24)))
            q += 4; return v
        }
        let stop = i32(), ended = i32(), delayMs = i32()
        return BotDrive(actions: actions, events: [], stopRaw: stop, ended: ended, delayMs: delayMs)
    }
}
