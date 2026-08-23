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

    /// THE COMPOSER'S READ: everything the bubble about to be staged says about
    /// itself - its header, and the line that goes under it.
    ///
    /// It lives here rather than in the extension because of the FIRST half.
    /// The payload is read with `peek`, which does NOT adopt: a `decode` also
    /// re-bases the kernel, telling it that the chain up to and including the
    /// move just staged is history somebody else made - so the next action of
    /// the same turn measured its delta (msg_wire.h's n_new) from the middle of
    /// its own bubble, and this caption then described only the tail of it. The
    /// base belongs to the chain the device ADOPTED (the bubble it opened, or
    /// its own bubble once sent - MessageTurnController.markSent); composing one
    /// must not move it. Keeping the read and the caption in one kit function is
    /// what lets a test walk the path the composer actually walks.
    ///
    /// `leftName` is the round-16 lobby case: the name of whoever this device
    /// just removed from the roster, which only this device can know (the join
    /// that carried it is what the leave took out).
    ///
    /// nil env means the payload would not parse at all - the caller stages the
    /// bytes anyway (they are its own seal), with the generic tap line.
    public static func forStagedBubble(payload: Data,
                                       leftName: String? = nil) async -> (env: MessageEnvelope?,
                                                                          summary: String) {
        let env = try? await MessageEnvelope.peek(payload: payload)
        // The board the seal left resident, spectator view (no hand) - so no
        // hand can leak into a notification line, and the fool announcement has
        // a source.
        let view = await MessageKernel.shared.residentView(viewer: -1)
        return (env, await line(env: env, view: view, leftName: leftName))
    }

    /// The line itself, for an already-read envelope. Pure over (env, view,
    /// leftName) plus the kernel's event stream for the bubble's own atoms.
    static func line(env: MessageEnvelope?, view: GameView?, leftName: String?) async -> String {
        let names = Dictionary((env?.joins ?? []).map { ($0.seat, $0.name) },
                               uniquingKeysWith: { a, _ in a })
        func seatName(_ seat: Int) -> String {
            names[seat] ?? FStrings.t("ios.msg.seatn", ["n": "\(seat + 1)"])
        }
        if env?.phase == 3 {
            // §12: the finished bubble announces the fool.
            let fool = view?.gameOver ?? -1
            return fool >= 0 ? FStrings.t("ios.msg.fool", ["name": seatName(fool)])
                             : FStrings.t("ios.msg.tap")
        }
        if env?.phase == 0 {
            // A WAITING lobby (§5.2): the creator's bubble invites the thread to
            // join; a later join re-seals with the joiner as last actor.
            //
            // Round 16 - a LEAVE reseals a lobby too, and reads nothing like a
            // join. Two things separate them, and both are needed: the leaver's
            // own device passes `leftName` (only it knows the name, since the
            // join carrying it is what was removed), and a leave's
            // `lastActorSeat` points at a free slot rather than a seated player,
            // which is how a RECEIVER tells the two apart with no name to go on.
            let joinCount = env?.joins.count ?? 0
            let actor = env?.lastActorSeat ?? -1
            let actorIsSeated = env?.joins.contains { $0.seat == actor } ?? false
            if let leftName { return FStrings.t("ios.msg.left", ["name": leftName]) }
            if !actorIsSeated && joinCount >= 1 { return FStrings.t("ios.msg.leftanon") }
            return joinCount > 1 ? FStrings.t("ios.msg.joined", ["name": seatName(actor)])
                                 : FStrings.t("ios.msg.joininvite")
        }
        if env?.phase == 2, env?.turn == 0 {
            // The last-joiner LIVE handoff carries no move yet - the game just
            // started; name who started it.
            return FStrings.t("ios.msg.started", ["name": seatName(env?.lastActorSeat ?? -1)])
        }
        // An ordinary live move: describe it from the kernel's event stream, for
        // the atoms THIS bubble added and no earlier ones (round 16's delta).
        let events = await MessageKernel.shared.lastMoveEvents(viewer: -1,
                                                              atomsBefore: env?.atomsBefore ?? -1)
        return move(events: events, names: names, view: view,
                    actor: env?.lastActorSeat ?? -1)
    }
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

    private static func cardList(_ cs: [Card]) -> String {
        cs.map(card).joined(separator: ", ")
    }

    private static func name(_ seat: Int, _ names: [Int: String]) -> String {
        names[seat] ?? FStrings.t("ios.msg.seatn", ["n": "\(seat + 1)"])
    }

    /// One headline action, kept as FACTS rather than a finished sentence, so
    /// that consecutive actions of the same kind by the same seat can be merged
    /// into ONE line at render time (round 12).
    ///
    /// A bubble is not one action: it carries the whole trailing run of steps
    /// its sender produced (see `MessageKernel.lastMoveEvents` - "a staged
    /// double cover replays both"). The old caption kept only the FIRST
    /// headline and dropped the rest, so a defender who covered two cards
    /// before sending was announced as covering one, and an attacker who threw
    /// in a second card was announced with the first (owner, round 12:
    /// "multiple cover does not result in full text explanation in bubble
    /// text, only one cover").
    private struct Beat {
        var seat: Int
        var kind: Int
        /// attack / pass: every card played across the merged actions.
        var cards: [Card] = []
        /// cover: one (beaten, beater) pair per merged action.
        var pairs: [(target: Card?, with: Card?)] = []
    }

    /// The summary for a staged LIVE move (phase 2, turn > 0). `events` is
    /// `MessageKernel.lastMoveEvents(viewer: -1)` for the resident game (the
    /// trailing run of steps the bubble's sender produced); `view` is the public
    /// resident board, for the round-over "whose turn" line; `actor` is the
    /// envelope's own lastActorSeat (who sealed this bubble). Falls back to the
    /// generic tap line if the stream yields no headline.
    public static func move(events: [GameEvent], names: [Int: String], view: GameView?,
                            actor: Int = -1) -> String {
        var beats: [Beat] = []
        var outParts: [String] = []
        var roundOver = false

        /// Append to the running beat when it is the same seat doing the same
        /// kind of thing; otherwise start a new one. Merging is only ever
        /// within a contiguous run, so "attack, cover, attack" could never
        /// collapse into one attack sentence.
        func headline(_ seat: Int, _ kind: Int, _ mutate: (inout Beat) -> Void) {
            if var last = beats.last, last.seat == seat, last.kind == kind {
                mutate(&last)
                beats[beats.count - 1] = last
            } else {
                var b = Beat(seat: seat, kind: kind)
                mutate(&b)
                beats.append(b)
            }
        }
        for e in events {
            switch e.msg {
            case Msg.attacked:
                headline(e.seat, Msg.attacked) { $0.cards += e.cards.compactMap { $0 } }
            case Msg.passed:
                headline(e.seat, Msg.passed) { $0.cards += e.cards.compactMap { $0 } }
            case Msg.covered:
                headline(e.seat, Msg.covered) {
                    $0.pairs.append((e.target, e.cards.compactMap { $0 }.first))
                }
            case Msg.pickup:
                headline(e.seat, Msg.pickup) { _ in }
            case Msg.out:
                if e.seat >= 0 { outParts.append(FStrings.t("ios.msg.mv.out", ["name": name(e.seat, names)])) }
            case Msg.goodTransition:
                roundOver = true
            default:
                break
            }
        }

        func sentence(_ b: Beat) -> String {
            switch b.kind {
            case Msg.attacked:
                return FStrings.t("ios.msg.mv.attack",
                    ["name": name(b.seat, names), "cards": cardList(b.cards)])
            case Msg.passed:
                return FStrings.t("ios.msg.mv.pass",
                    ["name": name(b.seat, names), "cards": cardList(b.cards)])
            case Msg.covered:
                let pairs = b.pairs.map {
                    FStrings.t("ios.msg.mv.coverpair", [
                        "target": $0.target.map(card) ?? "",
                        "card": $0.with.map(card) ?? ""])
                }.joined(separator: ", ")
                return FStrings.t("ios.msg.mv.cover",
                    ["name": name(b.seat, names), "pairs": pairs])
            default:
                return FStrings.t("ios.msg.mv.pickup", ["name": name(b.seat, names)])
            }
        }
        var primary: String? = beats.isEmpty ? nil : beats.map(sentence).joined(separator: " · ")

        // "Says good" is the ONE silent move: every other live action leaves a
        // headline event naming its seat (attack/pass/cover/pickup all emit
        // their EVW_MSG_*), but a good emits nothing of its own - evwire has no
        // said-good tag. Two shapes reach here (round-8 #2):
        //  - a BARE good (the bout stays open): the event stream is empty, no
        //    headline at all - the old caption fell to the generic tap line;
        //  - a round-CLOSING good: the wire encodes it as the seatless
        //    ROUND_END step, so the last-move grouping backs over the tail to
        //    the PREVIOUS actor's step - the headline says the other seat's
        //    cover (already captioned on its own bubble) and never that this
        //    bubble's sender said good.
        // Both give themselves away the same way: NO headline belongs to the
        // envelope's own lastActorSeat. The actor's action was the silent
        // good, so it takes the headline; the consequences (outs, the round
        // transition) still follow. All kernel facts, nothing re-derived.
        // (Round 12: asked of EVERY beat, not just the first - now that a
        // bubble can carry several, "the actor is in here somewhere" is the
        // question, and one merged run is still one seat's work.)
        if actor >= 0, !beats.contains(where: { $0.seat == actor }) {
            primary = FStrings.t("ios.msg.mv.good", ["name": name(actor, names)])
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
