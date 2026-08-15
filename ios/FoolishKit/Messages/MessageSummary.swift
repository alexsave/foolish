// MessageSummary — the human line under a game bubble (MSMessage.summaryText),
// added in 1.0(4). It describes the move the bubble carries, so the transcript
// and the notification read "Alex attacks with K of ♠", not a generic "tap to
// play".
//
// THE RULE holds here too: nothing about the move is re-derived. The facts come
// from the kernel's own evwire event stream (MessageKernel.lastMoveEvents, the
// SAME bytes the board animates and the website renders in
// sdk/ts/wire/evwire.ts reconstructMessage). Each event is already tagged with
// its message template (EVW_MSG_*); this file only turns that into a localized
// sentence with flat suit glyphs.

import Foundation

public enum MessageSummary {
    // EVW_MSG_* (c/src/evwire.h) — the kernel's per-event message tag.
    private enum Msg {
        static let attacked = 1, passed = 2, out = 3, covered = 4
        static let pickup = 8, goodTransition = 9
    }

    /// A flat card: "K of ♠" (rank + localized "of" + a monochrome suit glyph).
    /// Mirrors the web's cardDisplay, with the suit as a flat glyph rather than a
    /// word — the "flat suit emoji" the summary uses.
    public static func card(_ c: Card) -> String {
        guard !c.isHidden, let s = c.suit else { return "🂠" }
        return FStrings.t("ios.msg.cardfmt", ["rank": CardRank.label(c.v), "suit": s.glyph])
    }

    private static func cardList(_ cs: [Card?]) -> String {
        cs.compactMap { $0 }.map(card).joined(separator: ", ")
    }

    private static func name(_ seat: Int, _ names: [Int: String]) -> String {
        names[seat] ?? FStrings.t("ios.msg.seatn", ["n": "\(seat + 1)"])
    }

    /// The summary for a staged LIVE move (phase 2, turn > 0). `events` is
    /// `MessageKernel.lastMoveEvents(viewer: -1)` for the resident game (the
    /// trailing run of steps the bubble's sender produced); `view` is the public
    /// resident board, for the round-over "whose turn" line. Falls back to the
    /// generic tap line if the stream yields no headline.
    public static func move(events: [GameEvent], names: [Int: String], view: GameView?) -> String {
        var primary: String?
        var outParts: [String] = []
        var roundOver = false

        // First headline wins for `primary`; OUT and the round transition are
        // consequences appended after it (a defender who covers the last card
        // and thereby ends the bout reads "… covers … · Round over - X attacks").
        for e in events {
            switch e.msg {
            case Msg.attacked:
                primary = primary ?? FStrings.t("ios.msg.mv.attack",
                    ["name": name(e.seat, names), "cards": cardList(e.cards)])
            case Msg.passed:
                primary = primary ?? FStrings.t("ios.msg.mv.pass",
                    ["name": name(e.seat, names), "cards": cardList(e.cards)])
            case Msg.covered:
                let def = e.cards.compactMap { $0 }.first
                primary = primary ?? FStrings.t("ios.msg.mv.cover", [
                    "name": name(e.seat, names),
                    "target": e.target.map(card) ?? "",
                    "card": def.map(card) ?? ""])
            case Msg.pickup:
                primary = primary ?? FStrings.t("ios.msg.mv.pickup", ["name": name(e.seat, names)])
            case Msg.out:
                if e.seat >= 0 { outParts.append(FStrings.t("ios.msg.mv.out", ["name": name(e.seat, names)])) }
            case Msg.goodTransition:
                roundOver = true
            default:
                break
            }
        }

        var parts: [String] = []
        if let primary { parts.append(primary) }
        parts.append(contentsOf: outParts)
        if roundOver, let fa = view?.firstAttacker, fa >= 0 {
            parts.append(FStrings.t("ios.msg.mv.roundover", ["name": name(fa, names)]))
        }
        return parts.isEmpty ? FStrings.t("ios.msg.tap") : parts.joined(separator: " · ")
    }
}
