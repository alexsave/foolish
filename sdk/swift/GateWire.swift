// GateWire.swift - the chain layer's four gates, asked of the kernel.
//
// Is this board a branch off an old one, is this nickname usable, is it taken,
// and which seat am I. The rules are msg_wire.c's (msg_chain_is_ahead,
// msg_nickname_verdict, msg_name_taken, msg_seat_*) so a second chain client
// does not re-derive them; this file is the crossing, and it is the only place
// the `fio_*` gate entries are named.
//
// It exists as a file of its own rather than as an import in
// FoolishKit/Messages: the C bridge stays inside sdk/swift (§7.1, and
// ios/scripts/lint_architecture.sh enforces it), and the app layer spends typed
// Swift. What is left up there is the part no kernel should be doing - trimming
// whitespace, counting grapheme clusters, App Group storage, and the words the
// UI shows.
//
// Every roster crosses as RosterWire's packed bytes and every name as raw UTF-8
// with a length, never a C string: a nickname is arbitrary Unicode and NUL is
// not its terminator.

import Foundation
import CFoolish

public enum GateWire {

    // MARK: - the nickname

    /// What the kernel makes of a nickname's TRIMMED counts. The host does the
    /// Unicode work and hands over two numbers; both caps are the kernel's, so
    /// the UI limit and the one the seal enforces cannot drift apart.
    public enum NameVerdict: Int32, Sendable {
        case ok       = 0   // FIO_NAME_OK
        case empty    = 1   // FIO_NAME_EMPTY
        case tooLong  = 2   // FIO_NAME_TOO_LONG
    }

    /// MSG_MAX_NAME_CHARS - what a lobby row or a seat badge can show.
    public static var nameMaxChars: Int { Int(fio_name_max_chars()) }
    /// MSG_MAX_NAME - the cap the SEAL enforces, in UTF-8 bytes.
    public static var nameMaxBytes: Int { Int(fio_name_max_bytes()) }

    /// A trimmed name's verdict from its character and byte counts. A name can
    /// clear one cap and blow the other, which is the whole reason both cross.
    public static func nicknameVerdict(chars: Int, bytes: Int) -> NameVerdict {
        NameVerdict(rawValue: fio_nickname_verdict(Int32(chars), Int32(bytes))) ?? .empty
    }

    /// Is this (already-trimmed) name held by a seat in the roster? Exact match
    /// on the sealed string; a roster that does not read answers false.
    public static func nameTaken(_ name: String, in joins: [MessageJoin]) -> Bool {
        RosterWire.call(joins, name) { fio_roster_name_taken($0, $1, $2, $3) != 0 }
    }

    // MARK: - the branch

    /// MSG_PHASE_FINISHED - the phase a chain carries when its game is over.
    public static let finishedPhase = Int(FIO_PHASE_FINISHED)

    /// Does `a` show MORE of the game than `b`? Round is compared above turn
    /// and a TIE IS NOT AHEAD; msg_wire.h says why, and where it fails open.
    public static func chainIsAhead(phase a_phase: Int, round a_round: Int, turn a_turn: Int,
                                    thanPhase b_phase: Int, round b_round: Int,
                                    turn b_turn: Int) -> Bool {
        fio_msg_chain_is_ahead(Int32(a_phase), Int32(a_round), Int32(a_turn),
                               Int32(b_phase), Int32(b_round), Int32(b_turn)) != 0
    }

    // MARK: - the seat

    /// Which seat am I, from the three §6 signals? nil is ambiguous - not an
    /// error, but the only honest answer, which the caller turns into a picker.
    public static func seatResolve(cachedSeat: Int?, senderIsLocal: Bool, nPlayers: Int,
                                   lastActorSeat: Int, chatIsDM: Bool) -> Int? {
        let s = fio_seat_resolve(Int32(cachedSeat ?? -1), senderIsLocal ? 1 : 0,
                                 Int32(nPlayers), Int32(lastActorSeat), chatIsDM ? 1 : 0)
        return s >= 0 ? Int(s) : nil
    }

    /// Does this roster list my cached seat under a DIFFERENT name than the one
    /// this device recorded when it claimed it - a claim race this device lost?
    public static func seatCacheDisowned(cachedSeat: Int?, recordedName: String?,
                                         joins: [MessageJoin]) -> Bool {
        RosterWire.call(joins, recordedName) {
            fio_seat_cache_disowned($0, $1, Int32(cachedSeat ?? -1), $2, $3) != 0
        }
    }

    /// The seat carrying my own recorded claim name in this roster, or nil.
    public static func seatClaimedByName(recordedName: String?,
                                         joins: [MessageJoin]) -> Int? {
        let s = RosterWire.call(joins, recordedName) { fio_seat_claimed_by_name($0, $1, $2, $3) }
        return s >= 0 ? Int(s) : nil
    }

    /// `seatResolve` gated on this bubble's OWN roster - the lobby answer. nil
    /// covers both ambiguous and resolved-but-not-listed, which a lobby must
    /// not tell apart: neither one may act.
    public static func seatResolveInLobby(cachedSeat: Int?, senderIsLocal: Bool,
                                          nPlayers: Int, lastActorSeat: Int,
                                          chatIsDM: Bool, recordedName: String?,
                                          joins: [MessageJoin]) -> Int? {
        let s = RosterWire.call(joins, recordedName) {
            fio_seat_resolve_in_lobby($0, $1, Int32(cachedSeat ?? -1),
                                      senderIsLocal ? 1 : 0, Int32(nPlayers),
                                      Int32(lastActorSeat), chatIsDM ? 1 : 0, $2, $3)
        }
        return s >= 0 ? Int(s) : nil
    }
}
