// StaleBranchGate - may this board be played on, given what this device has
// already seen of the same game?
//
// The decision `GameSurface.rankAgainstHighWater` used to make inline, lifted
// out for the reason every other decision in this folder was lifted out
// (SeatIdentity, StagedBubbleRouting, MessageSurfaceRouter): it can be driven
// end to end from a test with real sealed chains, no simulator and no taps.
// The view now only spends the answer.
//
// THE BUG THIS TYPE EXISTS TO CLOSE, 1.0(40), owner on a real device:
//
//   "this game state somehow got me into an 'an older move - the game has
//    moved on' message. not sure how, but the move was perfectly legal.
//    Somehow I do see that there are two bubble previews, both saying 'eva
//    attacks with 8 of clubs'"
//
// Two bubbles in the thread carried the SAME move, and the board then refused
// a legal move as superseded. The two halves are one bug:
//
//   * A RE-SEAL STAMPS A FRESH SEND CLOCK (msg_wire.h's `sent_at`, and a
//     parent8/n_new that may also move), so one move sealed twice is two
//     byte-DIFFERENT payloads describing one identical game state. The
//     extension re-seals for perfectly ordinary reasons - `stageBaseNow`'s
//     undo-to-empty overwrite of a staged bubble is a re-seal of the adopted
//     chain and nothing else - and `conversation.insert` only replaces a draft
//     that has not been SENT yet, so a re-seal after a send is a second bubble
//     in the transcript rather than a replacement of the first.
//
//   * RULE P THEN ORDERS THAT PAIR, because Rule P is a TOTAL order and has
//     to be. Two same-state siblings tie on phase, round, turn and joins and
//     fall through to msg_rule_p's lexicographic digest tiebreak, which is a
//     coin flip; a same-state CHILD (the undo-to-empty re-seal names its
//     parent's digest in parent8) wins outright on rule 4. Either way
//     `preferred(mine, known) > 0` - and the gate read that as "the table has
//     moved past you" and turned the board read-only.
//
// "NOT NEWER" IS NOT "OLDER". Rule P answers WHICH of two chains to adopt, and
// for that a coin flip is fine and rule 4 is exactly right. It does not answer
// "has the game moved on without me", which is the only question this gate is
// asking, and the one the owner's board got wrong. So Rule P's verdict is kept
// as a NECESSARY condition and a second, independent one is added: the chain on
// file must actually show MORE OF THE GAME.
//
// What was tried and rejected:
//
//   * Suppressing the duplicate bubble (drop a re-seal that adds nothing).
//     Wrong layer. The undo-to-empty re-seal is the only way §10 can cancel a
//     staged move at all, and a board that refuses a legal move is broken
//     whatever produced the second bubble - including a genuine concurrency
//     fork nobody can suppress.
//   * Fixing msg_rule_p to return 0 for same-state chains. That would make it
//     a partial order, and every caller that must pick exactly one chain
//     (arrival adoption, the flow simulator's convergence) needs a total one.
//     Rule P is unchanged and stays the kernel's.
//   * Comparing the two decoded GAMES instead of their headers. A decode is an
//     ADOPTION (MessageEnvelope.peek's own doc), so ranking would move the
//     resident game out from under the board it is ranking. The header is what
//     may be read here, and `peek` is how.
//
// ROUND 43 - THIS TYPE ALSO OWNS THE END OF THE MARK'S LIFE. The owner, on the
// high-water map growing without bound: "easy - clear when the last move
// plays." This gate is where that happens, because it is already the only
// reader and the only writer of `MessageGameStore.latestChain`, and it is a
// pure enum a test can drive with real sealed chains. See `record` below.
//
// The kernel calls cross through sdk/swift/GateWire.swift (§7.1).
import Foundation

public enum StaleBranchGate {

    /// How much of the game a chain claims to contain, straight off the
    /// envelope header the kernel decoded. Nothing here is a rule: these are
    /// the three fields msg_wire.h stamps at seal time from the game the body
    /// actually replayed to, so a device cannot claim progress it did not make.
    public struct Progress: Equatable {
        /// 0 WAITING, 1 ACCEPT, 2 LIVE, 3 FINISHED.
        public let phase: Int
        /// Completed-round counter.
        public let round: Int
        /// Applied kernel actions (ATOMS, see the caveat on `isAhead`).
        public let turn: Int

        public init(phase: Int, round: Int, turn: Int) {
            self.phase = phase; self.round = round; self.turn = turn
        }

        public init(_ env: MessageEnvelope) {
            self.init(phase: env.phase, round: env.round, turn: env.turn)
        }
    }

    /// Does `known` show MORE of the game than `mine`? The kernel's
    /// (msg_chain_is_ahead), because a chain-based client that re-derived it
    /// would have to re-derive two things that are not obvious: that ROUND is
    /// compared above TURN, and that a TIE IS NOT AHEAD. See msg_wire.h for
    /// both, and for where it deliberately fails open.
    public static func isAhead(_ known: Progress, of mine: Progress) -> Bool {
        GateWire.chainIsAhead(phase: known.phase, round: known.round, turn: known.turn,
                              thanPhase: mine.phase, round: mine.round, turn: mine.turn)
    }

    /// The gate's answer: is this board a read-only branch, and what is the
    /// newer chain the bar offers to open?
    public struct Verdict: Equatable {
        /// Stand `iCanAct` / `canStage` down (MessageTurnController.setSuperseded).
        public let superseded: Bool
        /// The chain on file that beat this one, for the "Open newest" button.
        /// nil whenever `superseded` is false - the bar is not shown, and there
        /// is nothing to offer.
        public let newest: Data?
    }

    /// `MSG_PHASE_FINISHED`, from the kernel rather than written as a bare 3.
    ///
    /// The wire makes this claim CHECKABLE rather than merely stated: msg_wire.c
    /// decode refuses a chain whose header and body disagree about being over -
    /// `if (over && e->phase != MSG_PHASE_FINISHED) return MSG_EPHASE;` and the
    /// converse right under it. So a peeked phase of 3 is not a device's opinion
    /// about a game, it is a fact the kernel replayed the body to confirm, which
    /// is the standard anything gating storage against a possibly-hostile sender
    /// has to meet.
    static let finishedPhase = GateWire.finishedPhase

    /// Note `payload` as the newest chain seen for this game - OR, when the
    /// game it carries is over, drop the note entirely (round 43).
    ///
    /// WHY A FINISHED GAME'S MARK IS DEAD WEIGHT. The mark exists to answer one
    /// question - "may this board be staged on?" - and a finished board answers
    /// it without help: `iCanAct` is false, the legal menu is empty, there is no
    /// move to branch with. Keeping the row past that point buys nothing and
    /// costs a whole base64 chain in an App Group plist that is read on every
    /// bubble open, in an extension with a hard memory ceiling.
    ///
    /// WHAT IT COSTS, stated rather than glossed: once the row is gone, tapping
    /// an OLD mid-game bubble of that same finished game finds nothing on file
    /// and gets a live, playable board again. That is a real (if narrow) reopening
    /// of the round-20 hole, and it is the trade the owner asked for. It is
    /// bounded on the other side by Rule P, which is where the guarantee actually
    /// lives: a branch sealed off that board loses to the finished chain on every
    /// device that still holds one, so it converges away rather than replacing
    /// the ending. This gate was always a courtesy on the brancher's own device -
    /// its own header calls the note "purely an optimization" - never the thing
    /// that makes the fork illegal.
    ///
    /// Only the two paths that would RECORD go through here. The two that
    /// deliberately do not - a same-state sibling, and a genuine supersession -
    /// leave the row exactly where it stands, because in both of those the chain
    /// on file is the one Rule P prefers and it is not this board's to drop. Both
    /// are fork geometries, and both resolve the moment that preferred chain is
    /// itself opened: if it is finished, its own rank clears the row then.
    private static func record(_ payload: Data, env: MessageEnvelope,
                               chatKey: String, store: MessageGameStore) {
        guard env.phase != finishedPhase else {
            AnimLog.say("stale gate: game \(env.gameId) is over - the high-water mark is dropped")
            store.forgetLatestChain(gameId: env.gameId)
            return
        }
        store.setLatestChain(gameId: env.gameId, chatKey: chatKey, payload: payload)
    }

    /// IS THIS BOARD A BRANCH OFF AN OLD BUBBLE, and record it if it is not.
    ///
    /// Two authorities, both consulted, neither sufficient alone:
    ///   * the kernel's Rule P, against the newest chain this device has seen
    ///     for the same game (`MessageGameStore.latestChain`) - it says which
    ///     of the two the thread will converge on;
    ///   * `isAhead`, over the same two headers - it says whether converging
    ///     there costs this board anything.
    /// Only when BOTH say so does the board go read-only.
    ///
    /// FAILS OPEN, everywhere. Nothing on file, a store with no App Group, a
    /// kernel call that threw, a header that will not peek - all answer "not
    /// superseded" and record what they can.
    public static func rank(payload: Data, env: MessageEnvelope, chatKey: String,
                            store: MessageGameStore = .shared,
                            kernel: MessageKernel = .shared) async -> Verdict {
        guard let known = store.latestChain(gameId: env.gameId, chatKey: chatKey),
              known != payload else {
            record(payload, env: env, chatKey: chatKey, store: store)
            return Verdict(superseded: false, newest: nil)
        }
        // > 0 means the SECOND argument wins, so this asks "does what I already
        // have beat what is being opened?" - the same call `maybeAdoptIncoming`
        // makes, in the other direction.
        let pref = (try? await kernel.preferred(payload, known)) ?? -1
        guard pref > 0 else {
            record(payload, env: env, chatKey: chatKey, store: store)
            return Verdict(superseded: false, newest: nil)
        }
        // Rule P prefers the chain on file. The gate now asks the second
        // question, on the headers alone: peek, never decode - a decode would
        // ADOPT the chain on file and move the resident game out from under the
        // board being ranked.
        guard let knownEnv = try? await kernel.peek(payload: known) else {
            AnimLog.say("stale gate: the chain on file will not peek - board stays live")
            return Verdict(superseded: false, newest: nil)
        }
        guard isAhead(Progress(knownEnv), of: Progress(env)) else {
            // A DUPLICATE IS NOT A SUPERSESSION. Same phase, same round, same
            // turn: whatever Rule P's tiebreak said, the thread has not moved
            // past this board, and refusing a legal move here is the 1.0(40)
            // report. The mark on file is deliberately left where it stands -
            // Rule P prefers it, and overwriting it with an equal-rank sibling
            // would let the high-water flip-flop with every tap.
            AnimLog.say("stale gate: a newer-ranked chain carries the same state "
                + "(phase \(knownEnv.phase) round \(knownEnv.round) turn \(knownEnv.turn)) "
                + "- not a supersession, board stays live")
            return Verdict(superseded: false, newest: nil)
        }
        AnimLog.say("board is behind - a newer chain for game \(env.gameId) is on file")
        return Verdict(superseded: true, newest: known)
    }
}
