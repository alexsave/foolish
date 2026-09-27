// BotDriveWire.swift - one bot cycle's result, READ BY THE KERNEL.
//
// This file used to unpack a block ios_bots_api.c wrote for it: a u32 count, a
// four-byte head per action, two card runs and three little-endian ints. The
// cycle's result is a struct the kernel already has (bot_drive.h BotDriveOut),
// so it crosses as itself now and the reader is generated from that declaration
// (sdk/swift/gen/kernel.ios.swift, readBotDriveOut).
//
// The DELAY is not in the struct - it is a question about the drive rather than
// a field of it - so it comes back as fio_bot_drive's return value.
//
// Events are not carried (unused until B4 animation): the decoded BotDrive has
// an empty events list.

import Foundation
import FoolishKit
import CFoolishBots

public enum BotDriveWire {
    private static let types: [MoveType] = [.attack, .cover, .pass, .pickup, .good, .wait]

    /// The cycle the kernel just drove, with the delay it reported. Empty for a
    /// drive that did not read whole, which KernelLayout makes unreachable.
    public static func read(delayMs: Int) -> BotDrive {
        func empty() -> BotDrive { BotDrive(actions: [], events: [], stopRaw: 0, ended: -1, delayMs: 0) }
        guard let p = fio_bot_drive_ptr(), let d = try? readBotDriveOut(p) else { return empty() }
        let actions = d.actions.map { a -> BotAction in
            let type = (a.move.type >= 0 && a.move.type < types.count) ? types[a.move.type] : .unknown
            let cards = a.move.cards.map { Card(s: $0.suit, v: $0.value) }
            let attacks = a.move.attackCards.map { Card(s: $0.suit, v: $0.value) }
            return BotAction(seat: a.seat, type: type, cards: cards,
                             attackCards: type == .cover ? attacks : nil, pace: a.pacingClass)
        }
        return BotDrive(actions: actions, events: [], stopRaw: d.stop, ended: d.ended, delayMs: delayMs)
    }
}
