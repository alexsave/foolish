// AskWire.swift - everything the online app asks its server that is not a move.
//
// A move POSTs the awire action frame (MoveWire.encodeAction). The lobby edits
// and the bot nudge used to POST JSON objects this app spelled for itself
// ({ "type": "join", "game_id": ... }), which is a wire format stated in Swift,
// and one the server had to keep reading for as long as an app that sent it was
// installed. They are an ask now (c/src/ask.h): the kernel writes the request
// body through `fio_ask_encode`, the same kernel reads it on the server
// (`ask_decode`) and runs it (`table_ask`), and nothing in Swift knows a byte
// of it. The web sends the same bytes through the same encoder
// (sdk/ts/table/client_table.ts askBody).
//
// It lives in the SDK, though only the online app calls it, because CFoolish is
// imported nowhere else (ios/scripts/lint_architecture.sh).

import Foundation
import CFoolish

public enum AskWire {

    /// The asks the app sends. The numbers are the kernel's (ios_api.h FIO_ASK_*).
    public enum Kind {
        case join, start, addBot, exit, continueGame, bump

        var code: Int32 {
            switch self {
            case .join: return FIO_ASK_JOIN
            case .start: return FIO_ASK_START
            case .addBot: return FIO_ASK_ADD_BOT
            case .exit: return FIO_ASK_EXIT
            case .continueGame: return FIO_ASK_CONTINUE
            case .bump: return FIO_ASK_BUMP
            }
        }
    }

    /// The request body for an ask, written by the kernel.
    ///
    /// `bot`: the bot an add-bot names (nil: any bot not seated), or the bot an
    /// exit removes. `player`: the player an exit removes (nil of both: the
    /// caller leaves).
    ///
    /// nil for an ask the kernel will not write: an empty or over-long game id,
    /// an id longer than a roster id.
    public static func body(_ kind: Kind, gameId: String, bot: String? = nil, player: String? = nil) -> Data? {
        let g = chars(gameId), b = chars(bot ?? ""), p = chars(player ?? "")
        var out = [UInt8](repeating: 0, count: 512)
        let cap = Int32(out.count)
        let n: Int32 = g.withUnsafeBufferPointer { gp in
            b.withUnsafeBufferPointer { bp in
                p.withUnsafeBufferPointer { pp in
                    fio_ask_encode(kind.code,
                                   gp.baseAddress, Int32(gameId.utf8.count),
                                   bp.baseAddress, Int32((bot ?? "").utf8.count),
                                   pp.baseAddress, Int32((player ?? "").utf8.count),
                                   &out, cap)
                }
            }
        }
        return n > 0 ? Data(out.prefix(Int(n))) : nil
    }

    /// A string's UTF-8 as the kernel takes an id: bytes and a count, never a
    /// NUL. One zero byte of tail so an EMPTY id still has a non-nil
    /// baseAddress; the kernel is told the count separately and never reads it.
    private static func chars(_ s: String) -> [CChar] {
        s.utf8.map { CChar(bitPattern: $0) } + [0]
    }
}
