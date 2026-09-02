// NicknameGate — pure UI-side nickname validation, round-5 B1 (the UI half).
//
// The wire enforces its OWN byte cap at the seal layer (MSG_MAX_NAME,
// c/src/msg_wire.h) and rejects anything over it with MSG_ENAME — which
// `MessagesRootView`'s `createWaiting`/`joinLobby` used to turn into "this
// game link is damaged" (docs/APP_REVIEW_NOTES.md B1: an ordinary too-long
// name reads as a broken LINK, not a rejected name). B1's fix has three
// parts; this type is the first one — catch it in the UI so the seal layer
// is never handed a name it is going to reject in the first place.
//
// Two SEPARATE limits, and a name must clear BOTH ("if it goes over EITHER,
// disable the Create game [button]" — the owner's round-5 call):
//   - `maxBytes` mirrors the wire's own cap. It MUST equal MSG_MAX_NAME
//     (c/src/msg_wire.h) — that cap is being raised from 12 to 64 this same
//     round precisely so ordinary non-Latin names fit (B1's headline example,
//     "Владимир", is 8 characters but 16 UTF-8 bytes, and did not fit the old
//     12-byte cap). If the wire cap ever moves again, this constant moves
//     with it: too low silently rejects names the wire would accept, too high
//     lets one through for the seal to fail on — B1 again, just moved here.
//   - `maxChars` is a separate, UI-only display limit (16 characters) on top
//     of the byte cap: a name can clear 64 bytes and still be an unreasonably
//     long string of 1-byte characters that the lobby row / seat badge has no
//     room for.
//
// Deliberately no truncating case: the owner's call is to REJECT a name over
// either cap outright (dim the button, name the reason), never to silently
// cut it down to something that fits.
public enum NicknameGate {
    /// UI-only display cap — not derived from the wire, just what a lobby row
    /// or seat badge can show without eliding.
    public static let maxChars = 16
    /// Must equal MSG_MAX_NAME (c/src/msg_wire.h). Keeps the UI from ever
    /// handing the seal layer a name it is going to reject (round-5 B1).
    public static let maxBytes = 64

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

    /// Trim whitespace once, then check length. Character count first (the
    /// cap a human typing actually feels), byte count second — a string can
    /// clear the character cap and still blow the byte one (multi-byte
    /// scripts, or a short run of multi-codepoint emoji), and B1 is
    /// specifically about a cap that only ever looked at bytes catching
    /// ordinary short names nobody expected to fail.
    public static func check(_ raw: String) -> Verdict {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return .empty }
        if trimmed.count > maxChars { return .tooLong }
        if trimmed.utf8.count > maxBytes { return .tooLong }
        return .ok(trimmed)
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
        joins.contains { $0.name == name }
    }
}
