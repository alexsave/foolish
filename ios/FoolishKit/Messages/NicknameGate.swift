// NicknameGate - the nickname a lobby will accept, round-5 B1 (the UI half).
//
// The wire enforces its OWN byte cap at the seal layer (MSG_MAX_NAME,
// c/src/msg_wire.h) and rejects anything over it with MSG_ENAME — which
// `MessagesRootView`'s `createWaiting`/`joinLobby` used to turn into "this game
// link is damaged" (docs/APP_REVIEW_NOTES.md B1: an ordinary too-long name
// reads as a broken LINK, not a rejected name). B1's fix has three parts; this
// type is the first one - catch it in the UI so the seal layer is never handed
// a name it is going to reject.
//
// THE RULE IS THE KERNEL'S (msg_nickname_verdict / msg_name_taken). Both caps
// live beside the one the seal enforces, which is what stops them drifting: a
// UI cap set too low silently rejects names the wire would take, one set too
// high hands the seal a name it will refuse, and B1 is what that looks like on
// a device. What stays here is the Unicode work no C kernel should be doing -
// trimming whitespace, and counting CHARACTERS (grapheme clusters) rather than
// scalars or bytes.
//
// Two separate caps, and a name must clear BOTH ("if it goes over EITHER,
// disable the Create game [button]" - the owner's round-5 call): the wire's 64
// UTF-8 bytes, and a display limit of 16 characters that a lobby row or seat
// badge can actually show. Deliberately no truncating case: reject and name the
// reason, never silently cut a name down to something that fits.

import Foundation
import CFoolish

public enum NicknameGate {
    /// The display cap, from the kernel (MSG_MAX_NAME_CHARS) - read rather than
    /// repeated, so there is one number.
    public static var maxChars: Int { Int(fio_name_max_chars()) }
    /// MSG_MAX_NAME, the cap the SEAL enforces. Same reason.
    public static var maxBytes: Int { Int(fio_name_max_bytes()) }

    /// The three states a nickname field can be in, once trimmed. `.ok`
    /// carries the TRIMMED string — the exact value that gets stored and
    /// sealed, never the raw field text.
    public enum Verdict: Equatable {
        case ok(String)
        /// Blank after trimming — nothing entered, or only whitespace.
        case empty
        /// Over `maxChars` characters, or over `maxBytes` UTF-8 bytes (or
        /// both). Which one it was does not matter to the caller: the button
        /// text is the same either way ("nickname too long").
        case tooLong
    }

    /// Trim whitespace once, count the two ways, and let the kernel judge. A
    /// string can clear the character cap and still blow the byte one
    /// (multi-byte scripts, or a short run of multi-codepoint emoji), which is
    /// exactly what B1 was: a cap that only ever looked at bytes catching
    /// ordinary short names nobody expected to fail.
    public static func check(_ raw: String) -> Verdict {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        switch fio_nickname_verdict(Int32(trimmed.count), Int32(trimmed.utf8.count)) {
        case Int32(FIO_NAME_OK): return .ok(trimmed)
        case Int32(FIO_NAME_TOO_LONG): return .tooLong
        default: return .empty
        }
    }

    /// Is this (already-trimmed) name held by a seat in the lobby being
    /// joined? Names are the only identity a payload can carry (§6 — no
    /// account, and Apple's participant UUIDs neither transfer across devices
    /// nor belong in the wire), so the ghost-seat guard, the §6.3 picker, and
    /// the lobby's "(you)" tag all lean on them. They can only lean as far as
    /// names are unique WITHIN a chain — which nothing enforced: two "Alex"es
    /// in one lobby made the picker a coin flip and the disown check blind
    /// between them. The Join button refuses a taken name (exact match on the
    /// sealed, trimmed string), so any single chain's names stay distinct;
    /// forked chains can still each hold their own "Alex" — the residual the
    /// claim-token design (docs/IMESSAGE_SEAT_IDENTITY_V2.md) exists to close.
    public static func isTaken(_ name: String, in joins: [MessageJoin]) -> Bool {
        RosterWire.call(joins, name) { fio_roster_name_taken($0, $1, $2, $3) != 0 }
    }
}
