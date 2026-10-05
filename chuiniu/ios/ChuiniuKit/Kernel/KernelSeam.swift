// KernelSeam.swift - THE ONE SEAM between the screens and the kernel.
//
// Everything a screen draws is a field of `TableModel`, and everything a touch
// does is one method of `Kernel`. Swift decides no rule, composes no sentence
// and derives no game number: legality is `Menu`, which dice count at a
// reveal is `Reveal.counts`, every word is `Kernel.word` or a string field of
// the model. The screens never import the kernel module.
//
// THE ONE KERNEL is `BridgeKernel` (beside this file), which reads CChuiniu
// (chuiniu/c/ios/include/cn_api.h) through the generated readers
// (Generated/ChuiniuKernel.swift) into a TableModel. The scaffold's scripted
// FakeKernel is gone; a test builds real positions by driving the bridge.

import CoreGraphics
import Foundation

// MARK: - the model the screens draw

public enum Phase: Equatable, Sendable {
    /// Seats are filling; nobody has dice.
    case lobby
    /// A round is being bid.
    case bidding
    /// The last bid was called: every die is face up.
    case revealed
    /// One seat holds dice; the game is done.
    case over
}

/// A bid: `quantity` dice showing `face` (2 to 6) among every die on the table.
public struct Bid: Equatable, Sendable {
    public var quantity: Int
    public var face: Int
    public init(quantity: Int, face: Int) {
        self.quantity = quantity
        self.face = face
    }
}

public struct SeatModel: Equatable, Sendable, Identifiable {
    /// The seat index, 0 to 5, in seat order.
    public var id: Int
    public var name: String
    /// How many dice the seat still holds (public: every cup's count is seen).
    public var dice: Int
    public var alive: Bool
    public var isTurn: Bool
    public var isMe: Bool
    /// The kernel's lobby row for this phone ("2. Bo (You)"); the name alone
    /// is what a bubble every phone sees may show.
    public var lobbyRow: String
    public init(id: Int, name: String, dice: Int, alive: Bool, isTurn: Bool, isMe: Bool, lobbyRow: String = "") {
        self.id = id
        self.name = name
        self.lobbyRow = lobbyRow
        self.dice = dice
        self.alive = alive
        self.isTurn = isTurn
        self.isMe = isMe
    }
}

/// What the player on turn may do: the kernel's verdict, never Swift's.
public struct Menu: Equatable, Sendable {
    /// The lowest legal raise; the picker opens on it.
    public var minimumRaise: Bid
    /// For each face 2 to 6, the least quantity a raise on that face may name
    /// (index = face; 0 and 1 unused). A face with no legal raise has a value
    /// above `maxQuantity`.
    public var minQuantityByFace: [Int]
    /// The stepper's upper bound: every die on the table.
    public var maxQuantity: Int
    public var callAllowed: Bool

    public init(minimumRaise: Bid, minQuantityByFace: [Int], maxQuantity: Int, callAllowed: Bool) {
        self.minimumRaise = minimumRaise
        self.minQuantityByFace = minQuantityByFace
        self.maxQuantity = maxQuantity
        self.callAllowed = callAllowed
    }

    /// The least quantity the kernel allows on `face`, or nil for a face it
    /// did not rank. A lookup, not a rule.
    public func minQuantity(face: Int) -> Int? {
        minQuantityByFace.indices.contains(face) && face >= 2 ? minQuantityByFace[face] : nil
    }
}

/// Every die face up after a call.
public struct Reveal: Equatable, Sendable {
    /// Each seat's dice (index = seat; empty for a seat that is out).
    public var dice: [[Int]]
    /// Whether each of those dice counts toward the called bid (its face or a
    /// wild 1): the kernel's tally, drawn as it says.
    public var counts: [[Bool]]
    /// The bid that was called.
    public var bid: Bid
    /// The kernel's words for the tally ("There were five").
    public var tally: String
    /// The kernel's outcome line, once the call is sent (K8): "Bo calls. Four
    /// 3s was true, Bo loses a die".
    public var outcome: String
    /// The seat that loses a die.
    public var loser: Int
    /// This phone may go on to the next round's table (its own new dice,
    /// and the opener's menu). A look, never a move: nothing is sent.
    public var nextAllowed: Bool

    public init(dice: [[Int]], counts: [[Bool]], bid: Bid, tally: String, outcome: String, loser: Int,
                nextAllowed: Bool) {
        self.dice = dice
        self.counts = counts
        self.bid = bid
        self.tally = tally
        self.outcome = outcome
        self.loser = loser
        self.nextAllowed = nextAllowed
    }
}

/// The lobby's one control for this phone (shared/c/msg_lobby_roster's msg_lobby_roster_offered).
public enum LobbyOffer: Equatable, Sendable {
    case join, start, waiting, full, alone
}

public struct TableModel: Equatable, Sendable {
    public var phase: Phase
    public var seats: [SeatModel]
    /// My seat, nil for a phone that has not joined (a spectator).
    public var me: Int?
    /// My own dice this round, face values 1 to 6 (empty in the lobby).
    public var myDice: [Int]
    /// The bid on the table, nil before the round's opening bid.
    public var bid: Bid?
    /// The kernel's words for the bid ("four 3s"), "" when there is none.
    public var bidText: String
    /// Who made the bid on the table.
    public var bidder: Int?
    /// My staged raise, not yet sent (the committed table never shows it;
    /// only the bubble it goes out in does), and the kernel's words for it.
    public var stagedBid: Bid?
    public var stagedBidText: String
    /// Every seat's dice, once the bid is called (and at the end).
    public var reveal: Reveal?
    /// The kernel's headline for this phone, under the table ("Your turn:
    /// raise or call Liar", "Send to bid four 3s").
    public var caption: String
    /// The kernel's caption of the bubble the resident would stage now,
    /// shown in the transcript on every phone ("Alex bid four 3s").
    public var bubbleCaption: String
    /// My choices when it is my turn to bid; nil otherwise.
    public var menu: Menu?
    /// The lobby's control for this phone.
    public var offered: LobbyOffer
    /// Changes when the round's dice change, so the roll plays once per
    /// round. An identity, not a number anything is computed from.
    public var rollID: Int
    /// The winner, when over.
    public var winner: Int?

    public init(phase: Phase, seats: [SeatModel], me: Int?, myDice: [Int], bid: Bid?, bidText: String,
                bidder: Int?, stagedBid: Bid? = nil, stagedBidText: String = "", reveal: Reveal?,
                caption: String, bubbleCaption: String, menu: Menu?, offered: LobbyOffer, rollID: Int,
                winner: Int?) {
        self.phase = phase
        self.seats = seats
        self.me = me
        self.myDice = myDice
        self.bid = bid
        self.bidText = bidText
        self.bidder = bidder
        self.stagedBid = stagedBid
        self.stagedBidText = stagedBidText
        self.reveal = reveal
        self.caption = caption
        self.bubbleCaption = bubbleCaption
        self.menu = menu
        self.offered = offered
        self.rollID = rollID
        self.winner = winner
    }

    public static let empty = TableModel(phase: .lobby, seats: [], me: nil, myDice: [], bid: nil, bidText: "",
                                         bidder: nil, reveal: nil, caption: "", bubbleCaption: "", menu: nil,
                                         offered: .waiting, rollID: 0, winner: nil)
}

/// Every fixed word a screen shows. The kernel's string table answers
/// (cn_api_string, by key name).
public enum Word: CaseIterable, Sendable {
    case gameTitle
    case lobbyTitle, lobbyWaiting, lobbyFull
    case join, start
    case raise, call, nextRound
    case loses, out
    case namePrompt
}

/// The kernel's motion at one moment of a reveal (cn_api_beats_frame): what
/// the reveal screen draws instead of its settled look while the call's
/// beats play.
public struct RevealMotion: Equatable, Sendable {
    /// Every cup is up (the LIFT beat has run).
    public var cupsUp: Bool
    /// How many of the counting dice are lit so far, in seat order.
    public var lit: Int
    /// Every beat has run: draw the settled reveal.
    public var done: Bool
    public init(cupsUp: Bool, lit: Int, done: Bool) {
        self.cupsUp = cupsUp
        self.lit = lit
        self.done = done
    }
}

// MARK: - the kernel, as the screens and the conversation see it

/// One call per touch. A method that answers `true` has left a bubble to
/// stage (`stagedURL()`, captioned `table.bubbleCaption`).
@MainActor
public protocol Kernel: AnyObject {
    /// The resident game as the screens draw it, read fresh.
    var table: TableModel { get }
    func word(_ w: Word) -> String

    /// This device's participant identity in the conversation.
    func me(_ participant: Data)
    /// The nickname this device sits down under ("" for none).
    func nickname(_ name: String)
    /// The nickname it has now, for the name field to open on.
    var currentNickname: String { get }
    /// Who sent the tapped bubble `url` (nil clears the fact).
    func sender(_ url: URL?, isDM: Bool, iSent: Bool)
    /// The kernel's words for a refused link's error.
    func errorText(_ code: Int) -> String
    /// A new lobby with me in seat 0.
    func newGame(dm: Bool) -> Bool
    /// The kernel's verdict on a nickname typed into the lobby.
    func nameAccepted(_ name: String) -> Bool
    func join(name: String) -> Bool
    func start() -> Bool
    func raise(quantity: Int, face: Int) -> Bool
    func call() -> Bool
    func nextRound() -> Bool

    /// Adopt a bubble's link as the resident game: 0, or the kernel's
    /// negative error, and then nothing changed.
    func adoptBubble(_ url: URL) -> Int
    /// The link of the resident game, to stage.
    func stagedURL() -> URL?
    /// `url` is exactly the link my staged move writes: the resident is the
    /// truth, and the link must not be read over it (pickemup's keepsDraft).
    func keepsStaged(_ url: URL) -> Bool
    /// How long my staged move's own beats run, in ms (the collapse waits
    /// for them).
    var stagedSettleMs: Int { get }
    /// The reveal's motion `ms` after the newest plan began; nil when no
    /// reveal is playing.
    func revealMotion(atMs ms: Int) -> RevealMotion?
    /// When the newest plan began (the adopt or the send that built it).
    var motionStart: Date? { get }
    /// `url` went out: it is the authority (pickemup's didStartSending rule).
    func sent(_ url: URL)
    /// Messages' X on the staged bubble: the draft is taken back.
    func cancelStaged()
    /// Both links are one game.
    func sameGame(_ a: URL, _ b: URL) -> Bool
    /// `a` is later in the game than `b` (the kernel's ranking).
    func isNewer(_ a: URL, than b: URL) -> Bool
}

// MARK: the stage: the table's pixels, drawn by the kernel (cn_stage.h)

/// Which screen the stage draws (the kernel's CN_STAGE_*).
public enum StageScreen: Int {
    case table = 1, reveal = 2, bubble = 3
}

/// One frame: the picture and where it goes. The image goes at
/// `shot.canvas` (flat points: it turns with the planks by the HUD's `ca`).
/// The image is premultiplied BGRA, Core Animation's own form (it draws it
/// without redrawing it first).
public struct StageFrame {
    public let shot: CnStageShotSnap
    public let image: CGImage
}

/// The transcript picture: the stage's bubble frame and the HUD that places
/// the names and the plate on it (CN_STAGE_BUBBLE, 300 by 195 points).
public struct BubbleFrame {
    public let hud: CnStageHudSnap
    public let frame: StageFrame
}

/// THE TABLE'S PICTURE IS THE KERNEL'S. A host begins a screen of the resident
/// game and asks for frames on the clock it samples the beats on; every place
/// on the screen (cups, names, plate, shelf, my cup's tap target, the camera's
/// turn) is in the HUD, read through the generated reader. There is one stage
/// a process (the renderer is one).
///
/// MEMORY. The arena (CN_STAGE_ARENA, 48 MB) is taken by the first FRAME, not
/// by a begin, and given back by `purge` (a memory warning, the extension
/// going away) and after every bubble; the next frame takes it again and
/// draws the same picture. One stage, so one arena: the bubble and the live
/// table never hold two.
@MainActor
public protocol TableStage: AnyObject {
    /// Begin `screen` for a drawer of `drawer` points on a `scale` device;
    /// `roll` throws the round from the current plan's SHAKE beat. nil when
    /// the kernel has nothing to draw (or the readers are stale). `drawer` is
    /// the drawer as the host measured it (the hosting view's bounds), never
    /// a constant: the kernel lays the table out for it (cn_lay).
    func begin(_ screen: StageScreen, drawer: CGSize, scale: CGFloat, roll: Bool) -> CnStageHudSnap?
    /// The frame `ms` into the plan's clock, my cup tipped `peek` of its full
    /// tip; drawn in CN_STAGE_BANDS bands over the cores, now (the caller
    /// waits, behind any frame in flight). nil when nothing could be drawn.
    func frame(atMs ms: Int, peek: Double) -> StageFrame?
    /// THE DISPLAY'S FRAME, OFF THE MAIN THREAD: the same frame, its clock,
    /// peek and the resident's lift sampled now, drawn on the stage's own
    /// queue while the main thread goes on; `done` on the main actor with it
    /// (nil when nothing could be drawn). Frames are drawn one at a time, in
    /// the order asked; a begin, a purge or `frame` waits for the one in
    /// flight. The same `ms` and `peek` draw the same bytes either way.
    func submit(atMs ms: Int, peek: Double, then done: @escaping @MainActor (StageFrame?) -> Void)
    /// The same frame drawn on one thread, the reference the banded frame must
    /// equal byte for byte (the tests ask; no screen does).
    func frameOnOneThread(atMs ms: Int, peek: Double) -> StageFrame?
    /// The peek's tween at `t` (0 to 1 through CN_PEEK_MS): the kernel's ease.
    func peekEase(_ t: Double) -> Double
    /// Everything at rest at `ms`: the display link may stop.
    func done(atMs ms: Int) -> Bool
    /// A memory warning: the arena is freed; the next frame takes a new one
    /// and draws the same picture.
    func purge()
    /// THE BUBBLE: the resident game's transcript picture, drawn once at
    /// `scale` (the kernel clamps it: 2 at most), the arena freed after it.
    /// A table or reveal begun before is begun again exactly as it was (the
    /// same HUD, the same frame at the same time on its clock), so a screen
    /// on show never draws the bubble's table. nil when there is nothing to
    /// draw (a lobby: no dice yet).
    func bubble(scale: CGFloat) -> BubbleFrame?
    /// A SEAT'S NAME ON THE TABLE (cn_api_stage_name): its picture, drawn by
    /// the host (`NameDecal`), laid flat on the planks by the kernel in every
    /// frame of a table or a reveal from the next one on, so a cup in front of
    /// it hides it; nil takes it away. Hand it over only when it changes.
    func name(seat: Int, bitmap: NameBitmap?)
    /// The arena is allocated now.
    var holdsArena: Bool { get }
    /// The drawer the table or reveal on show was begun for, nil before one
    /// is: what the host measured, for a Debug check against its bounds.
    var drawer: CGSize? { get }
}

public enum KernelSeam {
    /// The kernel the extension runs on: the bridge.
    @MainActor
    public static func make() -> Kernel { BridgeKernel() }
    /// The stage that draws the table: the bridge's one.
    @MainActor
    public static func stage() -> TableStage { BridgeStage.shared }
}
