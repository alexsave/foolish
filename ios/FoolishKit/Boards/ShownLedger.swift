// ShownLedger - WHAT THE BADGES ARE SHOWING, and who is allowed to say so.
//
// Five pieces of board state answer "what number / mark / collapse is on
// screen right now", as distinct from what the kernel says: the deck count,
// the discard count, each seat's hand count, which seats are drawn as OUT, and
// which seats are wearing which role mark. They exist because a count must
// never move before the cards that earn it have flown - see each field below.
//
// THEY BELONG TO WHOEVER IS ANIMATING. A running sequence freezes them to the
// board BEFORE its move and walks them forward one step per landing flight
// (`MessageTableView.runEventStream`), then hands them back in its teardown. A
// caller that is NOT that sequence must not write them: doing so snaps every
// badge to a value the cards on screen have not earned yet, and the stream's
// next step puts it back. That is a visible count twitch with no move behind
// it, and it is the only kind the hand's own machinery cannot explain.
//
// WHY A TYPE, AND WHY IN ITS OWN FILE. Rounds 42 and 43 were the same bug
// twice. Round 42 found two writers with no ownership check (`freezeCounts`
// and `flyBoutEndToDiscard`'s `releaseCounts`) and gave each a copy of the
// same `guard BoardAnimator.sequenceDepth == 0` line. Round 43 then found a
// THIRD (`releaseLivePlayVeil`) making the very same writes with no guard at
// all - so round 42's fix was undone one line after it was applied - and the
// remedy was a third copy of the same line. With twenty-odd write sites spread
// over six thousand lines, correctness rested on every author of every future
// one remembering a rule written down in three places. A fourth writer was a
// matter of time.
//
// So the five fields are `private` here, in a file the board cannot reach
// into, and the ONLY way to change them is `write(_:)`, which takes a CLAIM
// saying who the caller is. That is a compiler-enforced funnel, not a
// convention: a new writer in MessageTableView.swift cannot assign to these
// fields at all, and cannot call `write` without picking a claim off the list
// below and reading what each one means. `CountOwnershipTests` pins the rest -
// that the funnel stays a funnel (one mutation point in this file), and that
// the four groups of callers still claim what they are.
//
// REJECTED: a `didSet` observer or a wrapper that guarded EVERY write. The
// rule is not "guard every write" - three of the four claims below are always
// allowed, and guarding the owner's own per-step advance would freeze every
// badge on the board permanently. The thing that had to become impossible was
// writing WITHOUT SAYING WHICH, and that is what a required parameter does.
//
// ONE SPELLING FOR THE PREDICATE. "Is a sequence running" is
// `BoardAnimator.isSequencing`, and this file is the only place that asks it
// about ownership. `sequenceDepth` stays for the three things that are about
// the NUMBER: claiming (`+= 1` / `-= 1`), the nested-wait floor in
// `MessageTableView.drainOtherSequences`, and printing it in a trace.
import Foundation

/// WHO IS WRITING. Every ledger write names one, and only `.bystander` ever
/// stands down - the other three are the owner in one of its three shapes.
/// The raw values are the kernel's claim codes (FIO_CLAIM_*, pinned by
/// CountOwnershipTests): the RULE over them is `ShownWrite.allows`
/// (c/src/anim_plan.c's anim_shown_ledger_allows), because a second client
/// drawing this board must stand its bystanders down on the same terms.
enum ShownClaim: Int, Equatable {

    /// THE RUNNING SEQUENCE, advancing or releasing its OWN ledger.
    ///
    /// `runEventStream` after it has claimed `animSequenceToken`: the per-step
    /// count advance, the deck/badge drop as cards LEAVE, the out-badge
    /// collapse, the cold-open seeds, and both releases (the empty-stream
    /// guard and the teardown). This is the owner by definition and is never
    /// refused - a guard here would freeze every badge for the life of the
    /// board, which is a far worse defect than the twitch.
    case sequence = 0

    /// ARMING A LEDGER FOR A SEQUENCE THAT IS ABOUT TO START, synchronously,
    /// before it exists to claim anything.
    ///
    /// `replayLastMoveOnOpen` only. It seeds the pre-move counts and roles in
    /// the same tick the board is first painted, because the board's first
    /// paint is this call's NEXT paint - a seed one Task hop later renders the
    /// final board first and then yanks it backwards. The sequence it is
    /// arming does not take `animSequenceToken` until `runEventStream` runs,
    /// so at this instant `isSequencing` may still describe somebody ELSE's
    /// stream.
    ///
    /// WHICH IS EXACTLY WHY IT IS NOT A BYSTANDER. An arrival landing while a
    /// previous sequence is still in flight is the normal case on an open
    /// board (`MessagesRootView.seatOnBoard` hands it to the live controller
    /// rather than rebuilding), and refusing the seed there would open the
    /// replay against the previous stream's MID-STATE - the board silently
    /// starting from the wrong numbers, which no count twitch would even hint
    /// at. The write is safe because it is immediately followed by the stream
    /// that owns it: that stream claims the token, walks these counts forward
    /// from exactly here, and hands them back in its teardown.
    case arming = 1

    /// A ROLE HAND-OFF: `syncRoles`, and nothing else.
    ///
    /// Unlike every other writer, this one is DIFF-AWARE - it reads what the
    /// badges are wearing, works out which marks actually changed hands, and
    /// FLIES them (`FRoleMotion`). It is the sanctioned way `roleShown` moves
    /// once it exists, which is why `freezeCounts` only ever seeds it.
    ///
    /// Always allowed, and that is a decision rather than an oversight. Its
    /// callers inside a stream are the owner; its ONE caller outside is the
    /// `!sequenced` branch of the board's `onChange`, which is the only thing
    /// in the file that animates a PASS's shield hand-off (a pass does not end
    /// a bout, so it starts no sequence and no closing beat ever runs for it).
    /// Making that a bystander would silently drop the shield's flight whenever
    /// a pass landed on a board that happened to be animating - a hand-off that
    /// teleports instead, with no test anywhere to notice.
    case handOff = 2

    /// EVERYBODY ELSE: a live play of mine, a refusal at a door, a rejection
    /// the kernel reports. `freezeCounts`, `releaseCounts`,
    /// `releaseLivePlayVeil`.
    ///
    /// Refused while a sequence is running, and it loses nothing by it: a play
    /// that lands here started no sequence and claimed no `animSequenceToken`
    /// (there is nothing to claim until the kernel publishes a table slot), so
    /// the stream still running is still the newest one, it is still walking
    /// these counts forward, and its teardown still hands them back. A board at
    /// rest - the common case, and every other caller - writes exactly as it
    /// always did.
    case bystander = 3
}

/// The board's shown-state ledger. Read freely; write only through `write`.
struct ShownLedger {

    /// THE FIVE FIELDS, and the only place in the app they can be assigned.
    /// Kept `internal` so the closure `write` hands out can set them; reachable
    /// only from inside this file, because `ShownLedger.fields` is private and
    /// nothing else vends a `Fields`.
    struct Fields {

        /// The deck count the well is drawing. nil = follow the kernel.
        var deck: Int?

        /// The discard count the pile is drawing. nil = follow the kernel.
        var discard: Int?

        /// Per-seat hand counts the badges are drawing. My own seat is
        /// deliberately absent: my hand is the fan, not a badge, and its cards
        /// are held by `animator.preHide` / `handHoldback` instead.
        var hand: [Int: Int] = [:]

        /// ROUND 28: WHICH SEATS THE BOARD IS DRAWING AS OUT, which during a
        /// sequence is not the same question as which seats ARE out.
        ///
        /// The badge of a player who goes out collapses edge-on and stays there
        /// (`FSeatBadge.collapsed`), and the owner's rule for WHEN is "in
        /// parallel with the card motion" - a player only ever goes out by
        /// playing their last cards, so the collapse and those flights are one
        /// event. The view carrying `isOut` arrives before the sequence starts,
        /// though, so read straight off it every badge would collapse a whole
        /// sequence early - the same lag the counts and the role marks each
        /// already keep, for the same reason.
        ///
        /// nil means "no sequence is running, follow the view", which is also
        /// what draws a seat that was ALREADY out when the board opened as
        /// collapsed from the first paint with nothing to animate: an out
        /// player is a fact about the board, and only the MOMENT of going out
        /// is an event.
        var out: Set<Int>?

        /// ROUND 16: the ROLES lag the game state the same way the counts do,
        /// and for the same reason. A bout end publishes one view in which the
        /// table is already clear, the hands already refilled AND the roles
        /// already rotated; the counts have been unpicked from that for rounds,
        /// but the marks still teleported - the shield was simply somewhere
        /// else on the first paint of a sequence whose cards had not begun to
        /// move. Now the badges wear this until the sequence that earns the
        /// change has played, and the change itself is a flight (FRoleMotion).
        var roles: MessageTableView.RoleState?

        // MARK: seeding

        /// A SEED IS NOT AN OVERRIDE, and the difference is the whole of round 16.
        ///
        /// `roles` and `out` are "what the badges are WEARING". Once they exist
        /// they are only ever advanced by something that knows what changed and
        /// can fly it - `syncRoles` for the marks, the per-step advance for the
        /// outs. Writing a fresh board over them would erase exactly that: a
        /// move played while a sequence is still animating (an impatient tap,
        /// the harness's auto-move) would set the marks to a board that has
        /// ALREADY rotated, and the hand-off the sequence was about to play
        /// would find nothing left to hand over.
        ///
        /// So every path that establishes them does it ONLY WHEN THEY ARE UNSET.
        /// Round 43: that rule was written out four times - twice in
        /// `runEventStream`, once in `freezeCounts`, once in
        /// `replayLastMoveOnOpen` - each with its own `if x == nil`. The SOURCES
        /// legitimately differ (a cold open asks the kernel for the board before
        /// the bubble's move; a live freeze already holds the pre-move view), so
        /// those stay at the call sites. The RULE does not differ, and lives
        /// here.
        ///
        /// `outs` is false where only the marks are being established: a live
        /// `freezeCounts` writes the out badges as an OVERRIDE in the same
        /// breath (it holds the pre-move board, so there is nothing to lag),
        /// while a cold open has no freeze behind it and must seed them or a
        /// bubble whose move puts somebody out opens with the badge already
        /// collapsed and nothing left to watch.
        ///
        /// Takes the BOARD, not a built value: a caller that had to construct a
        /// `RoleState` before asking would pay for it on every call, and the
        /// common case is that the seed is declined.
        mutating func seedMarks(from prior: GameView?, outs: Bool) {
            guard let prior else { return }
            if roles == nil { roles = MessageTableView.RoleState(prior) }
            if outs, out == nil { out = Set(prior.players.filter(\.isOut).map(\.seat)) }
        }
    }

    private var fields = Fields()

    // Reading is free and unguarded - the rule is entirely about who WRITES.
    var deck: Int? { fields.deck }
    var discard: Int? { fields.discard }
    var hand: [Int: Int] { fields.hand }
    var out: Set<Int>? { fields.out }
    var roles: MessageTableView.RoleState? { fields.roles }

    /// THE RULE, asked of the kernel (`ShownWrite.allows`), so it can be
    /// asserted without a board or a running sequence and so a second client
    /// cannot answer it differently. Only a bystander ever stands down; see
    /// `ShownClaim` for why each of the other three is the owner.
    static func allows(_ claim: ShownClaim, sequencing: Bool) -> Bool {
        ShownWrite.allows(claim: claim.rawValue, sequencing: sequencing)
    }

    /// THE ONE WAY TO CHANGE WHAT THE BADGES ARE SHOWING.
    ///
    /// - `claim`: who you are. Pick one off `ShownClaim` and read what it
    ///   means; if none of the first three describes you, you are a bystander.
    /// - `by` / `note`: the deferral trace, which is the only output on the
    ///   refused path. `note` is an autoclosure, so a caller may build an
    ///   expensive line (the whole badge row) without paying for it on the
    ///   common path where nothing is refused.
    ///
    /// Returns whether the write happened, for the callers that have something
    /// of their OWN to hand back alongside it (`releaseLivePlayVeil`'s swept
    /// table) and must do so on exactly the same terms.
    /// `@MainActor` because `BoardAnimator.sequenceDepth` is: the board's whole
    /// animation state lives on the main actor, and the ownership question is
    /// only meaningful there. Every caller is a `MessageTableView` method, which
    /// is main-actor by way of `View`.
    @MainActor
    @discardableResult
    mutating func write(_ claim: ShownClaim,
                        by who: String = "",
                        note: @autoclosure () -> String = "",
                        _ body: (inout Fields) -> Void) -> Bool {
        guard Self.allows(claim, sequencing: BoardAnimator.isSequencing) else {
            AnimLog.say("\(who) deferred (depth=\(BoardAnimator.sequenceDepth)) - \(note())")
            return false
        }
        body(&fields)
        return true
    }
}
