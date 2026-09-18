// KernelLayout.swift - the one check that stands between the generated readers
// in sdk/swift/gen and the library they read.
//
// Those readers are byte OFFSETS into structs the kernel owns, generated from
// c/src by tools/structgen under the iOS caps and the iOS triple. The library
// they read is not built with them: it is prebuilt into an xcframework by
// `make ios-lib` and linked as a binary. So the two can come apart - a header
// edit with no `make ios-lib`, or an `ios-lib` with no `tools/structgen/gen.sh`
// - and nothing about the mismatch is visible at compile time, because a byte
// offset is just a number. Every field would still read; some would read the
// wrong bytes.
//
// So both sides carry the layout's hash, and this compares them. The library's
// is baked in at build time (-DSG_LAYOUT_HASH, fio_layout_hash); the module's is
// SG_LAYOUT_HASH, written by the generator. A build that stamped none answers 0,
// which matches no module.
//
// The same gate the web has kept since Phase 1, where it runs at instantiate
// (sdk/ts/gen/layout_hash.bots.ts, checked in bots.ts). Here there is no
// instantiate, so it runs at the first call into the engine.

import Foundation
import CFoolish

public enum KernelLayout {

    /// The layout the linked library was compiled for, and the one these
    /// bindings were generated for. Equal, or the two are not a pair.
    public static var library: UInt32 { fio_layout_hash() }
    public static var bindings: UInt32 { SG_LAYOUT_HASH }
    public static var matches: Bool { library == bindings }

    /// What to do about a mismatch, said once rather than at every call site.
    ///
    /// It TRAPS. A wrong offset is not a degraded read a caller can fall back
    /// from: the fields it returns are the bytes of other fields, and every rule
    /// downstream - whose turn it is, which cards are on the table, which seat
    /// is the fool - is then computed from them. A crash on the first call, with
    /// the two numbers and the command that fixes it, is the smallest failure
    /// this can have; a board drawn from another field's bytes is the largest.
    ///
    /// It is checked once and remembered: the answer cannot change inside a
    /// process, and this sits in front of the whole bridge.
    public static let verified: Bool = {
        if matches { return true }
        fatalError("""
            The Foolish kernel and its generated Swift bindings are not a pair.
              library  (Foolish.xcframework): \(hex(library))
              bindings (sdk/swift/gen):       \(hex(bindings))
            Rebuild both from this tree:
              cd c && make ios-lib
              bash tools/structgen/gen.sh
            """)
    }()

    private static func hex(_ v: UInt32) -> String {
        "0x" + String(v, radix: 16, uppercase: false)
    }
}
