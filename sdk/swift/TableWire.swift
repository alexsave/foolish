// TableWire.swift - the one table layout, written by the kernel.
//
// Every kernel entry that takes a table takes the same 2 bytes per battle -
// the attack, then its cover or the no-card byte - and Swift used to write that
// layout in four places (PlayWire, ConflictWire, PreTableWire twice). The four
// agreed about every board the kernel can produce and disagreed about the one
// it cannot: a card on the table the viewer is not allowed to see. Two spelled
// it as "no card", which is how a covered battle turns into an open one - Good
// withheld, a drop target offered - with nothing refusing, because the wire is
// well-formed. One refused the whole board. One wrote the right byte.
//
// Now none of them writes a table byte. The kernel does (`fio_table_encode`,
// c/ios/ios_api_anim.c), from the SUIT/VALUE PAIRS a board already holds, and
// the rule for the masked card is stated once, beside the bytes it is stated
// in (ios_api.h). What is left here is the mapping from a BattleView onto
// those pairs, and ios/Fixtures/table_goldens.bin pins it.

import Foundation
import CFoolish

public enum TableWire {

    /// The kernel's table bytes for `battles`: 2 per battle, the attack then
    /// its cover, with the bare and unnameable sentinels chosen in C.
    ///
    /// THE BATTLE COUNT A CALLER SENDS BESIDE THESE IS `count / 2`, never
    /// `battles.count`. The two agree whenever the kernel wrote what it was
    /// asked - which, the encoder being total over content, is always - but a
    /// caller that trusted its own count over the bytes would hand the kernel
    /// a length its buffer does not have, and that is an over-read.
    public static func encode(_ battles: [BattleView]) -> [UInt8] {
        let pairs = self.pairs(battles)
        var out = [CChar](repeating: 0, count: 2 * battles.count)
        let n: Int32 = pairs.withUnsafeBufferPointer { p in
            fio_table_encode(p.baseAddress, Int32(battles.count), &out, Int32(out.count))
        }
        guard n >= 0 else { return [] }
        return out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
    }

    /// The pairs: each card's own fields, and for a battle with no cover the
    /// kernel's bare pair. NOTHING IS EXAMINED HERE - not whether a card is
    /// hidden, not whether it is on the deck - because deciding what such a
    /// card crosses as IS the encoding, and the encoding is the kernel's.
    /// `clamping` so a nonsense card reaches the kernel to be called
    /// unnameable rather than trapping on the way.
    ///
    /// Four zero bytes of tail so an EMPTY table still has a base address; the
    /// kernel is told the count separately and never reads them.
    static func pairs(_ battles: [BattleView]) -> [Int8] {
        var out: [Int8] = []
        out.reserveCapacity(4 * battles.count + 4)
        for b in battles {
            out.append(Int8(clamping: b.attack.s))
            out.append(Int8(clamping: b.attack.v))
            if let d = b.defense {
                out.append(Int8(clamping: d.s))
                out.append(Int8(clamping: d.v))
            } else {
                out.append(Int8(FIO_CARD_NONE))
                out.append(Int8(FIO_CARD_NONE))
            }
        }
        out.append(contentsOf: [0, 0, 0, 0])
        return out
    }
}
