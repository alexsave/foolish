// EngineC+Bots.swift - the bot half of the Swift bridge.
//
// A SEPARATE MODULE (FoolishBots) on purpose, and the reason is what ships.
// Foolish.xcframework is a static archive, so ld links it per object; FoolishKit
// is a dynamic framework, so its public API is exported and therefore a
// dead-strip root. While `botDrive` was a public method ON FoolishKit's EngineC,
// that export linked fio_bot_drive_packed -> bot_drive -> bot_roster -> all 21
// strategies, octogen included. FoolishKit ships inside FoolishMessagesApp, so
// octogen shipped in the iMessage bundle - a game that plays people and never
// drives a seat carried the strongest brain in the roster.
//
// Now the C is two archives (c/Makefile IOS_CORE_SRC / IOS_BOTS_SRC) and the
// Swift is two modules. The extension links FoolishKit alone and there is
// nothing for it to name. The host app links both.
//
// These are still methods on EngineC, not a wrapper: the kernel has ONE static
// resident game (fio_resident_game, c/ios/ios_internal.h) and EngineC is the
// actor that serializes access to it. A second type with its own queue would be
// two doors onto one room.

import Foundation
@_spi(FoolishBots) import FoolishKit
import CFoolishBots

extension EngineC {
    public func setSeatStrategy(seat: Int, strategyId: Int) throws {
        try Self.check(fio_set_seat_strategy(Int32(seat), Int32(strategyId)))
    }

    /// Run one bot cycle (docs/C_CORE_CONSOLIDATION.md F2/F3): the kernel picks
    /// fairly among simultaneously-eligible bots, applies 0..n actions, bundles
    /// the silent ones, and hands back how long to wait. `humanSeats` are the
    /// seats the kernel must not drive — note that a human being able to act is
    /// NOT a stop condition, so bots throw in while the player deliberates,
    /// exactly as they do online.
    public func botDrive(humanSeats: [Int]) throws -> BotDrive {
        var mask: Int32 = 0
        for s in humanSeats where s >= 0 { mask |= (1 << Int32(s)) }
        return BotDriveWire.decode(try json { fio_bot_drive_packed(mask, $0, $1) })
    }

    // MARK: strategies

    public func strategyCount() -> Int { Int(fio_strategy_count()) }

    /// The offline bot roster (id + name). The strategy table is static C data
    /// with no game state, so this is safe to read synchronously off the actor —
    /// the Home/bot-picker needs it before any game exists (§6, §7.2).
    public nonisolated static func roster() -> [(id: Int, name: String)] {
        var out: [(id: Int, name: String)] = []
        let n = Int(fio_strategy_count())
        var buf = [CChar](repeating: 0, count: 64)
        for i in 0..<n {
            let w = fio_strategy_name(Int32(i), &buf, 64)
            let name = w > 0 ? String(decoding: buf.prefix(Int(w)).map { UInt8(bitPattern: $0) }, as: UTF8.self) : "bot \(i)"
            out.append((id: i, name: name))
        }
        return out
    }

    public func strategyName(_ id: Int) throws -> String {
        let d = try json { fio_strategy_name(Int32(id), $0, $1) }
        return String(decoding: d, as: UTF8.self)
    }
}
