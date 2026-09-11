// StagedDraft - what a staged lobby bubble CLAIMS, and what the X on it goes
// back to. 1.1(69).
//
// Both of those used to be two `@State` fields on a private SwiftUI view with
// the rule that relates them written inline, which is why nothing could test
// it and two defects sat in it at once:
//
//   U11. THE RECORD-ONCE RULE WAS KEYED ON THE WRONG FIELD. `stagedParent8`
//        was doing double duty as "has this draft already recorded where it
//        came from", and a CREATE records an origin while having no parent at
//        all - a brand new chain has no earlier link in the thread to claim.
//        So the very next edit of the same draft found a nil parent, fell
//        through, and overwrote the create's `.noGame` origin with the lobby
//        the create had just made. Create, toggle passing, X landed you back
//        on that lobby: no Start, no Leave, no Join, and in a Release build
//        nothing on screen that can send. The two questions are now asked
//        separately, each of the field that answers it.
//
//   U2.  AN ORIGIN IS NOT ALWAYS A DELTA. `msg_surface_delta` refuses to diff
//        two chains whose game ids differ - deliberately, and correctly: two
//        different games share no line of continuity and animating between
//        them would be a lie (c/src/msg_wire.c). A REMATCH mints a fresh
//        random game id, so the chain it was created over is exactly that
//        case, and the plan came back with no beats and a ZERO settle - which
//        the caller reads as the owner's no-op rule and returns having done
//        nothing at all. `StagedRevert` names that third answer, so it can be
//        played as what it is: a whole-surface SWAP, the same beat the create
//        and its mirror already use (`anim_surface_swap`).
//
// NOTHING HERE DECIDES A TIMING OR AN IDIOM. Which beat a swap wears, how long
// it lasts, and whether a delta is worth playing at all are all still the
// kernel's. This only answers WHICH QUESTION to ask it.

import Foundation

/// WHAT THE STAGED DRAFT WAS STARTED FROM - the surface an X discards back to.
///
/// AND IT IS NOT ALWAYS A CHAIN. Every lobby action edits a table the thread
/// already has, so `.chain` covers them - but CREATE makes the game, and before
/// it this thread had no game at all. A brand new chain has no parent to record,
/// which is why a create never goes near `parent8(staged:digest:threadChain:)`
/// and why an X over a create used to revert nothing. The absence is therefore
/// recorded as a VALUE rather than as a nil that reads as "nothing was staged".
enum StagedOrigin: Equatable, Sendable {
    /// The table as everyone else still has it.
    case chain(Data)
    /// There was no game here. The X goes back to the New game screen.
    case noGame

    var payload: Data? {
        if case .chain(let d) = self { return d }
        return nil
    }
}

/// WHAT AN X ON THE STAGED BUBBLE HAS TO DO - the routing, and only the
/// routing.
///
/// Three destinations, because there are three genuinely different situations
/// and the fourth (`nothing`) is the guard that used to be two `guard` lines.
/// The one that did not exist before 1.1(69) is `swap`, and its absence is U2:
/// a base belonging to a DIFFERENT game reached the delta arm, diffed to no
/// beats and a zero settle, and was discarded as a no-op - leaving the human on
/// a control-less orphan lobby with the bubble already deleted.
enum StagedRevert: Equatable, Sendable {
    /// Nothing this surface staged is waiting to be undone.
    case nothing
    /// The draft MADE the game, so discarding it takes the game with it: back
    /// to the New game screen it was created from.
    case newGame
    /// A chain of a DIFFERENT game - a rematch's result card, say. There is no
    /// continuity to animate along, so the surface is swapped wholesale.
    case swap(Data)
    /// The same game, one state earlier. Ask the kernel what changed and play
    /// it backwards; a zero settle here really is the no-op rule.
    case delta(Data)

    /// `showingGameId` is what is on the surface right now, `baseGameId` the
    /// origin's. Both are the ENVELOPE's game id, peeked rather than decoded -
    /// naming a chain must not adopt it.
    ///
    /// A missing `showingGameId` (nothing on screen, or a header that will not
    /// read) is a surface that cannot be diffed against anything, which is the
    /// swap case too - and the honest one, since the alternative is the silent
    /// return U2 was.
    static func route(staged: Bool, origin: StagedOrigin?,
                     showingGameId: String?, baseGameId: String?) -> StagedRevert {
        guard staged, let origin else { return .nothing }
        guard let base = origin.payload else { return .newGame }
        guard let baseGameId, let showingGameId, baseGameId == showingGameId else {
            return .swap(base)
        }
        return .delta(base)
    }
}

/// The staged draft's two facts, kept in ONE value because they are recorded
/// together, cleared together, and were only ever wrong when they got out of
/// step (see the file header, U11).
struct StagedDraft: Equatable, Sendable {

    /// What the outgoing bubble claims as its ancestry. Nil while the draft has
    /// no parent to claim, which is a create rather than an absence.
    private(set) var parent8: Data?

    /// Where an X goes back to. Non-nil is the whole of "this draft has
    /// recorded its origin"; nothing else may be read for that.
    private(set) var origin: StagedOrigin?

    init() {}

    /// A CHAIN WITH NO ANCESTRY. `msg_seal`'s zero parent - what a create
    /// sends, and therefore what every later edit of that same unsent draft
    /// must go on sending. Claiming the created lobby's digest instead would
    /// name a link the thread has never seen.
    static let genesisParent8 = Data(repeating: 0, count: 8)

    /// The bubble was sent, or the surface was rebuilt: this draft is over.
    mutating func clear() {
        parent8 = nil
        origin = nil
    }

    /// A CREATE (or a rematch): a brand new chain, sealed with the genesis
    /// parent, whose origin the caller knows and this cannot infer. A create
    /// reached from the New game screen goes back to `.noGame`; a rematch was
    /// reached from the result card of the game just played, and goes back to
    /// THAT - a different game, which is why `StagedRevert` has a swap arm.
    mutating func created(from origin: StagedOrigin) {
        parent8 = nil
        self.origin = origin
    }

    /// ONE EDIT OF A LOBBY DRAFT: the parent8 to seal with.
    ///
    /// While a draft is already staged, my own intermediate chains are edits to
    /// THAT draft and not links anyone else can see, so the parent stays the one
    /// the thread has - and for a create, the one the thread has is nothing.
    ///
    /// The FIRST edit of a draft records the origin and later edits of the same
    /// draft do not, so a Join, a rules move and a Start staged together revert
    /// as ONE, exactly as the single bubble the X deletes carried them.
    @MainActor
    mutating func parent8(staged: Bool, digest: String, threadChain: Data?) -> Data {
        if staged, origin != nil { return parent8 ?? Self.genesisParent8 }
        let p = MessageTurnController.firstEight(hex: digest)
        parent8 = p
        origin = threadChain.map(StagedOrigin.chain) ?? .noGame
        return p
    }
}
