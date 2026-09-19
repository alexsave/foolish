// PackedAction.swift — the `action` endpoint's binary body (§8.1, §16.D3).
// Every online move POSTs one:
//   supabase.functions.invoke("action", body: <these bytes>)
// and the response is the 7-byte envelope decoded below.
//
// THE MOVE INSIDE IT IS THE KERNEL'S, not this file's. The awire action frame
// used to be written out here as well, a second Swift copy of the layout beside
// sdk/swift/MoveWire.swift's - and the two had already drifted on what a
// malformed move does. Both now call `MoveWire.encodeAction`, which asks
// `fio_awire_encode` (c/src/awire.c). What is left here is the REQUEST
// ENVELOPE around it, which is a server protocol rather than a game format: its
// twin is `encodeActionRequest` in sdk/ts/wire/awire.ts and its reader is
// `table_request_decode` in c/src/table.c.
//
// The move frame is pinned by golden vectors generated FROM THE KERNEL -
// ios/Fixtures/action_goldens.bin, written by c/ios/ios_action_goldens.c and
// checked fresh in CI - so the Swift mapping from a Move to those bytes is
// measured against C rather than against itself (ios/FoolishTests/
// PackedActionTests.swift).

import Foundation
import FoolishKit

public enum PackedAction {

    // Request envelope format bytes.
    public static let reqFormatV2: UInt8 = 2     // current: carries intent version
    // Response envelope.
    public static let respFormat: UInt8 = 1

    public enum Status: UInt8 { case applied = 0, rejected = 1, moot = 2 }
    /// Edge-policy reject code that sits above the kernel's 0..21 (the stale-round
    /// guard from WEB_RACE_BUG_HANDOFF.md §5). Surfaced with its own localized copy.
    public static let rejectStaleRound: UInt8 = 100

    // MARK: move → wire

    /// The move buffer: [kind][n][card×n], with cover appending [attackCard×n]
    /// - written by the kernel, not here.
    ///
    /// Throws `unencodableMove` for exactly what `awire_encode` refuses: a move
    /// with no action on the wire (`wait`, `unknown`), more cards than
    /// AWIRE_MAX_CARDS, a pickup or good carrying cards, or a cover whose attack
    /// cards do not pair up with its cover cards. ONE verdict, because there is
    /// one judge; the three separate errors this used to raise were this file's
    /// own reading of rules it did not own.
    public static func encode(_ move: Move) throws -> [UInt8] {
        let bytes = MoveWire.encodeAction(move)
        guard !bytes.isEmpty else { throw WireError.unencodableMove }
        return bytes
    }

    /// The full HTTP request body (format v2): [2][gid_len][gid][intentVersion:u32 LE][wire].
    /// `intentVersion` is the client's `games.version` intent — the stale-round
    /// guard compares it server-side (WEB_RACE_BUG_HANDOFF.md §5).
    public static func requestBody(gameId: String, intentVersion: UInt32, move: Move) throws -> Data {
        let gid = Array(gameId.utf8)
        guard gid.count <= 255 else { throw WireError.gameIdTooLong }
        var out: [UInt8] = [reqFormatV2, UInt8(gid.count)]
        out.append(contentsOf: gid)
        out.append(contentsOf: leU32(intentVersion))
        out.append(contentsOf: try encode(move))
        return Data(out)
    }

    // MARK: response envelope

    public struct Response: Equatable {
        public let status: Status
        public let rejectCode: UInt8
        public let version: UInt32
        public var isStaleRound: Bool { status == .rejected && rejectCode == PackedAction.rejectStaleRound }
    }

    /// Decode the 7-byte response [fmt=1][status][rejectCode][version:u32 LE].
    public static func decodeResponse(_ data: Data) throws -> Response {
        guard data.count >= 7 else { throw WireError.shortResponse }
        let b = [UInt8](data)
        guard b[0] == respFormat else { throw WireError.badResponseFormat }
        guard let status = Status(rawValue: b[1]) else { throw WireError.badStatus }
        let version = UInt32(b[3]) | (UInt32(b[4]) << 8) | (UInt32(b[5]) << 16) | (UInt32(b[6]) << 24)
        return Response(status: status, rejectCode: b[2], version: version)
    }

    // MARK: helpers

    private static func leU32(_ v: UInt32) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8((v >> 24) & 0xFF)]
    }

    public enum WireError: Error, Equatable {
        /// The kernel would not write this move as an action frame.
        case unencodableMove
        case gameIdTooLong, shortResponse, badResponseFormat, badStatus
    }
}
