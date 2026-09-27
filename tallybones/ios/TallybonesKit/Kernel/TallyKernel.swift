// TallyKernel.swift - THE SEAM between the Swift shell and the C kernel.
//
// Everything the screens (TallyTable, the views) and the conversation
// (MessagesViewController) ask of the game goes through this one protocol,
// which is pickemup's `Pk` bridge enum turned into a protocol so the shell can
// build and run before tallybones/c exists. Today the only conformer is
// StandInKernel (a fixed hand of dice, no rules); the integration worker adds
// the bridge-backed conformer beside it and swaps the one line in
// TallybonesHost.init that picks it.
//
// KERNEL: write `BridgeKernel: TallyKernel` in Kernel/BridgeKernel.swift over
// `import CTallybones` (tb_api.h) and Generated/, one call per method, in the
// shape of pickemup/ios/PickemupKit/Kernel/Pk.swift. Every method below says
// which pickemup entry point it mirrors.
//
// THE T11 CONTRACT a conformer must keep: `view()` over a resident with a
// pending KEEP draft reports the rerolling dice as 0 (unknown), and nothing
// here, or anywhere else, takes a draft and returns dice values for it. The
// values appear only after `commit()` (the send echo) or `adopt` of a sent
// bubble, when the resident replay derives them.

import Foundation

/// A move went into the draft: stage it (pickemup's TableModel.Stage).
public struct TallyStage: Equatable, Sendable {
    public let caption: String
    /// The drawer collapses by itself once the move has rested.
    public let collapse: Bool
    /// When it may, in ms from the stage (pickemup: the kernel's settle).
    public let settleMs: Int

    public init(caption: String, collapse: Bool, settleMs: Int = 750) {
        self.caption = caption
        self.collapse = collapse
        self.settleMs = settleMs
    }
}

/// The fixed words of the screens (T8: the kernel's word table). The
/// sentences with numbers or names in them come composed inside TallyView.
public enum TallyString: CaseIterable, Sendable {
    case lobbyTitle, join, start, leave, lobbyWaiting, lobbyAlone, lobbyFull, namePrompt, unreadable
    case roll, numbersSum, bonus, total, cardTitleMine, close
}

@MainActor
public protocol TallyKernel: AnyObject {

    // MARK: identity (pk_api_me, pk_api_sender, pk_api_nickname)

    /// This device's participant id in the conversation (16 bytes).
    func identify(me participant: Data)
    /// Who sent the tapped bubble, when one is tapped.
    func sender(of text: String?, isDM: Bool, iSent: Bool)
    func setNickname(_ name: String)
    /// The kernel's verdict on a typed name (pk_api_name_verdict == OK).
    func nameIsOK(_ name: String) -> Bool

    // MARK: the seat records (pk_api_seats_load, pk_api_seats_dirty)

    func loadSeats(_ bytes: Data?)
    func seatsIfDirty() -> Data?

    // MARK: the resident message (pk_api_read, pk_api_adopt, pk_api_text)

    /// The build's library and generated readers are one layout (D46).
    var layoutMatches: Bool { get }
    /// The resident's link, draft included, or nil when nothing is resident.
    var text: String? { get }
    /// A draft of mine is open in the resident.
    var hasDraft: Bool { get }
    /// Adopt `text` as the resident: 0, or a negative error and nothing
    /// changed. `arrival`: it landed while the board was up.
    func adopt(_ text: String, arrival: Bool) -> Int
    /// The staged draft went out (didStartSending): it is resident history
    /// now, and a KEEP's reroll is derived from it (T11).
    func commit()
    /// Messages' X on the staged bubble: the draft is gone.
    func cancelDraft()
    /// A new lobby with me in seat 0.
    func newGame(dm: Bool) -> Bool
    func sameGame(_ a: String, _ b: String) -> Bool
    /// <= 0 when `a` ranks at least as new as `b` (Rule P, pk_api_prefer).
    func prefer(_ a: String, over b: String) -> Int

    // MARK: reads (pk_api_view, pk_api_words)

    func view() -> TallyView
    func string(_ s: TallyString) -> String
    func categoryName(_ c: Category) -> String
    /// The words for a refused read's error code.
    func errorLine(_ code: Int) -> String
    var inviteCaption: String { get }

    // MARK: moves: each opens (or replaces) the draft and says how to stage it

    /// May the unkept dice be rerolled with these marks? (mask 31, all kept,
    /// is not a move: T7.)
    func mayRoll(keeping mask: Int) -> Bool
    /// KEEP(mask): the unkept dice go blank until the bubble is sent (T11).
    func roll(keeping mask: Int) -> TallyStage?
    /// SCORE(category): the last bubble of the turn.
    func score(_ c: Category) -> TallyStage?
    func join() -> TallyStage?
    func leave() -> TallyStage?
    func start() -> TallyStage?
}

/// The error a refused read reports when the library and the readers
/// disagree (pickemup's PK_EFORMAT).
public let tallyErrorFormat = -2
