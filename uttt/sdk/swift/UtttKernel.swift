import CUttt
import CoreGraphics
import Foundation

/// The kernel, and nothing else.
///
/// Every rule and every coordinate in this game comes from C. This file is the
/// boundary and answers no question of its own - if a method here computes
/// something rather than forwarding it, that is a bug in the same way a
/// hand-written byte reader would be.
public enum Uttt {

    public enum Mark: UInt8 { case none = 0, x = 1, o = 2, draw = 3 }

    // MARK: the game

    public static func newGame(seed: Int32) { uti_new(seed) }

    @discardableResult
    public static func play(_ move: Int) -> Bool { uti_play(Int32(move)) != 0 }

    /// The rulebook's text, straight from the kernel: a title and six lines.
    /// The renderer lays them out and writes none of them.
    public static var rulesTitle: String { String(cString: uti_rules_title()) }
    public static var rules: [String] {
        (0..<Int(uti_rules_count())).map { String(cString: uti_rules_line(Int32($0))) }
    }

    /// Take back the last move. False when there was none.
    @discardableResult
    public static func undo() -> Bool { uti_undo() != 0 }

    public static var legal: [UInt8] {
        var buf = [UInt8](repeating: 0, count: 81)
        let n = buf.withUnsafeMutableBufferPointer { uti_legal($0.baseAddress) }
        return Array(buf.prefix(Int(n)))
    }

    public static var over: Mark    { Mark(rawValue: UInt8(uti_over())) ?? .none }
    public static var turn: Mark    { Mark(rawValue: UInt8(uti_turn())) ?? .none }
    public static var forced: Int   { Int(uti_forced()) }
    public static var plyCount: Int { Int(uti_n_plies()) }
    public static func move(at i: Int) -> Int { Int(uti_move_at(Int32(i))) }
    public static func block(_ b: Int) -> Mark {
        Mark(rawValue: UInt8(max(0, uti_block(Int32(b))))) ?? .none
    }

    /// The whole game, in about twenty bytes. The bubble carries this.
    public static var code: Data {
        var buf = [UInt8](repeating: 0, count: 64)
        let n = buf.withUnsafeMutableBufferPointer {
            uti_encode($0.baseAddress, 64)
        }
        return n > 0 ? Data(buf.prefix(Int(n))) : Data()
    }

    @discardableResult
    public static func load(_ code: Data, seed: Int32) -> Bool {
        code.withUnsafeBytes { raw in
            uti_decode(raw.bindMemory(to: UInt8.self).baseAddress,
                       Int32(code.count), seed) != 0
        }
    }


    // MARK: the message
    //
    // What a bubble carries and who may do what with it are the kernel's
    // (uttt/c/src/uttt_msg.h). This side hands over an opaque string and gets
    // one back: no byte, tag or hash crosses. The resident message IS the
    // resident game - every accessor above reads its board.

    /// Where this device sits on the resident message. The numbers are the
    /// kernel's own macros, never retyped here.
    public enum Seat {
        case spectator, x, o
        /// My invitation, and nobody has taken it.
        case waiting
        /// Somebody's invitation: X is mine to take, with my first move.
        case open

        init(_ v: Int32) {
            switch v {
            case UTI_SEAT_X:       self = .x
            case UTI_SEAT_O:       self = .o
            case UTI_SEAT_WAITING: self = .waiting
            case UTI_SEAT_OPEN:    self = .open
            default:               self = .spectator
            }
        }
    }

    /// Who this device is: bytes the kernel hashes into a seat tag per game.
    public static func me(_ id: Data) {
        id.withUnsafeBytes { raw in
            uti_me(raw.bindMemory(to: UInt8.self).baseAddress, Int32(id.count))
        }
    }

    /// Messages' participant identifier, as the identity bytes.
    public static func me(participant: UUID) {
        me(withUnsafeBytes(of: participant.uuid) { Data($0) })
    }

    /// A new invitation from me, composed now. The moment is the seed.
    public static func openInvitation(at date: Date = Date()) {
        uti_msg_open(Int64(date.timeIntervalSince1970))
    }

    /// Adopt a message. False if it is not one this build reads, and then
    /// nothing changed.
    @discardableResult
    public static func read(_ text: String) -> Bool { uti_msg_read(text) == 0 }

    /// Whether `text` is a message this build reads, without adopting it.
    public static func readable(_ text: String) -> Bool { uti_msg_check(text) == 0 }

    /// The resident message, as the text a bubble carries.
    public static var messageText: String? {
        var buf = [CChar](repeating: 0, count: Int(UTI_MSG_TEXT_MAX))
        let n = buf.withUnsafeMutableBufferPointer {
            uti_msg_text($0.baseAddress, Int32(UTI_MSG_TEXT_MAX))
        }
        return n > 0 ? String(cString: buf) : nil
    }

    public static var seat: Seat { Seat(uti_msg_seat()) }
    /// The mark this device plays, or `.none`.
    public static var myMark: Mark { Mark(rawValue: UInt8(uti_msg_mark())) ?? .none }
    public static var seed: Int32 { uti_msg_seed() }
    public static var canMove: Bool { uti_msg_can_move() != 0 }

    /// Play as me. On an open invitation this TAKES THE SEAT with the move.
    @discardableResult
    public static func playAsMe(_ move: Int) -> Bool { uti_msg_play(Int32(move)) != 0 }

    /// Take back my own last move. Taking back the joining move gives the
    /// seat back.
    @discardableResult
    public static func undoMine() -> Bool { uti_msg_undo() != 0 }
    /// A CHANGE OF MIND: may I replace my staged move with `move` - another
    /// free square where the draft was played. Pure; every other tap on a
    /// board with a draft does nothing.
    public static func canReplace(_ move: Int) -> Bool { uti_msg_can_replace(Int32(move)) != 0 }

    /// The one door a screen may offer, and whether it may offer one at all:
    /// the kernel's rule (utm_door). Today that is Again, at the end.
    public enum Door { case none, again }
    public static var door: Door { uti_msg_door() == UTI_DOOR_AGAIN ? .again : .none }

    /// Which to show: true for `mine` (the staged draft), false for `tapped`.
    public static func prefersMine(_ mine: String, over tapped: String) -> Bool {
        uti_msg_prefer(mine, tapped) <= 0
    }

    public static func sameGame(_ a: String, _ b: String) -> Bool {
        uti_msg_same_game(a, b) != 0
    }

    /// Seal the resident game with these two identities in O and X. Only the
    /// debug harness and the preview can reach a game this way.
    @discardableResult
    public static func seat(o: Data, x: Data) -> Bool {
        o.withUnsafeBytes { ob in
            x.withUnsafeBytes { xb in
                uti_msg_seat_ids(ob.bindMemory(to: UInt8.self).baseAddress, Int32(o.count),
                                 xb.bindMemory(to: UInt8.self).baseAddress, Int32(x.count)) != 0
            }
        }
    }

    /// The move under a point in the board's 0..1 space, or nil off it.
    public static func hit(_ p: CGPoint) -> Int? {
        let mv = uti_hit(Float(p.x), Float(p.y))
        return mv >= 0 ? Int(mv) : nil
    }

    /// The square `mv` covers in the board's 0..1 space - the rectangle
    /// `hit` maps back to it.
    public static func cellRect(_ mv: Int) -> CGRect {
        var r: [Float] = [0, 0, 0, 0]
        guard uti_cell_rect(Int32(mv), &r) != 0 else { return .zero }
        return CGRect(x: CGFloat(r[0]), y: CGFloat(r[1]),
                      width: CGFloat(r[2]), height: CGFloat(r[3]))
    }

    /// What VoiceOver reads on square `mv`: "Top left board, centre square, empty".
    public static func sayCell(_ mv: Int) -> String { String(cString: uti_say_cell(Int32(mv))) }

    // MARK: the words

    /// Every sentence the app says, from the kernel's table (uttt_say.h).
    public struct Say {
        let key: Int32
        public static let bubbleHeadline = Say(key: UTI_SAY_BUBBLE_HEADLINE)
        public static let bubblePlace = Say(key: UTI_SAY_BUBBLE_PLACE)
        public static let caption = Say(key: UTI_SAY_CAPTION)
        public static let headlinePre = Say(key: UTI_SAY_HEADLINE_PRE)
        public static let headlinePost = Say(key: UTI_SAY_HEADLINE_POST)
        public static let subline = Say(key: UTI_SAY_SUBLINE)
        public static let watchLabel = Say(key: UTI_SAY_WATCH_LABEL)
        public static let watchLine = Say(key: UTI_SAY_WATCH_LINE)
        public static let waitingHeadline = Say(key: UTI_SAY_WAITING_HEADLINE)
        public static let waitingSubline = Say(key: UTI_SAY_WAITING_SUBLINE)
        public static let unreadableHeadline = Say(key: UTI_SAY_UNREADABLE_HEADLINE)
        public static let unreadableSubline = Say(key: UTI_SAY_UNREADABLE_SUBLINE)
        public static let youAre1 = Say(key: UTI_SAY_YOU_ARE_1)
        public static let youAre2 = Say(key: UTI_SAY_YOU_ARE_2)
        public static let doorAgain = Say(key: UTI_SAY_DOOR_AGAIN)
        public static let headlineSpoken = Say(key: UTI_SAY_HEADLINE_SPOKEN)
        public static let youAreSpoken = Say(key: UTI_SAY_YOU_ARE_SPOKEN)
        public static let doorRules = Say(key: UTI_SAY_DOOR_RULES)
        public static let sendHint = Say(key: UTI_SAY_SEND_HINT)
        public static let doorSend = Say(key: UTI_SAY_DOOR_SEND)
    }

    public static func say(_ s: Say) -> String { String(cString: uti_say(s.key)) }

    // MARK: getting a staged bubble into the field (uttt_msg.h)

    /// How long a staged bubble sits unsent before the send hint shows.
    public static var sendHintSeconds: Double { Double(uti_send_hint_ms()) / 1000 }
    /// How long an insert may go unanswered before its silence means something.
    public static var insertSilenceSeconds: Double { Double(uti_insert_silence_ms()) / 1000 }

    /// What an insert's silence means on try `attempt` (1-based).
    public enum InsertSilence { case listen, retry, door }
    public static func insertSilence(attempt: Int, compact: Bool) -> InsertSilence {
        switch uti_insert_silence(Int32(attempt), compact ? 1 : 0) {
        case UTI_INSERT_RETRY: return .retry
        case UTI_INSERT_DOOR:  return .door
        default:               return .listen
        }
    }

    /// The mark the bubble's headline draws before its words, or `.none`.
    public static var bubbleMark: Mark { Mark(rawValue: UInt8(uti_say_bubble_mark())) ?? .none }

    /// The mark drawn inside the play-surface headline, or `.none`.
    public static var sayMark: Mark { Mark(rawValue: UInt8(uti_say_mark())) ?? .none }
    /// `say` and `sayMark` of the position one ply back: what a screen says
    /// until the last move's ink has landed.
    public static func sayBefore(_ s: Say) -> String { String(cString: uti_say_before(s.key)) }
    public static var sayMarkBefore: Mark { Mark(rawValue: UInt8(uti_say_mark_before())) ?? .none }

    // MARK: the drawing

    /// A polygon the kernel wants filled. Points are 0..1 on both axes.
    public struct Poly {
        public let points: [CGPoint]
        public let color: CGColor
    }

    private static func harvest(_ count: Int32) -> [Poly] {
        guard count > 0, let pts = uti_points(), let polys = uti_polys() else { return [] }
        var out: [Poly] = []
        out.reserveCapacity(Int(count))
        for i in 0..<Int(count) {
            let f = Int(polys[i].first), n = Int(polys[i].n)
            var p: [CGPoint] = []
            p.reserveCapacity(n)
            for k in 0..<n {
                p.append(CGPoint(x: CGFloat(pts[(f + k) * 2]),
                                 y: CGFloat(pts[(f + k) * 2 + 1])))
            }
            let c = polys[i].rgba
            out.append(Poly(points: p, color: CGColor(
                red:   CGFloat((c >> 24) & 0xff) / 255,
                green: CGFloat((c >> 16) & 0xff) / 255,
                blue:  CGFloat((c >>  8) & 0xff) / 255,
                alpha: CGFloat( c        & 0xff) / 255)))
        }
        return out
    }

    /// The whole board. Expensive - thousands of polygons - so a caller
    /// rasterises this once per position and caches the image.
    public static func board(active: Int, last: Int,
                             markT: Float = 1, metaT: Float = 1) -> [Poly] {
        harvest(uti_draw(Int32(active), Int32(last), markT, metaT))
    }

    /// THE WHOLE BOARD, HANDED OVER: the kernel's own buffers for one draw
    /// (`uti_take`), owned here until the last reference goes, so it can be
    /// filled anywhere - including off the main thread, which the kernel's
    /// one resident display list must never be touched from. Nothing is
    /// copied and the kernel keeps nothing resident (TESTFLIGHT_PLAN.md 12).
    public final class BoardPolys: @unchecked Sendable {
        let taken: UtiTaken
        init(_ t: UtiTaken) { taken = t }
        deinit { uti_taken_free(taken) }
        public var count: Int { Int(taken.n_polys) }
    }

    public static func boardPolys(active: Int, last: Int) -> BoardPolys {
        harvestBoard(uti_draw(Int32(active), Int32(last), 1, 1))
    }

    /// The board as the bubble draws it: the main lines stop on its frame.
    public static func bubbleBoardPolys(active: Int, last: Int) -> BoardPolys {
        harvestBoard(uti_draw_bubble(Int32(active), Int32(last)))
    }

    private static func harvestBoard(_ count: Int32) -> BoardPolys {
        _ = count
        return BoardPolys(uti_take())
    }

    /// Fill `polys` into `cg` with the unit square scaled to `side`: the same
    /// polygons in the same order, one fill each, the colour set only when it
    /// changes. Pure - it reads nothing but its arguments.
    public static func fill(_ polys: BoardPolys, into cg: CGContext, side: CGFloat) {
        let t = polys.taken
        guard t.n_polys > 0, let pts = t.points, let q = t.polys else { return }
        let np = Int(t.n_points)
        var colour: UInt32 = 0
        var haveColour = false
        for i in 0..<Int(t.n_polys) {
            let f = Int(q[i].first), n = Int(q[i].n)
            guard n > 0, f + n <= np else { continue }
            let c = q[i].rgba
            if !haveColour || c != colour {
                cg.setFillColor(red: CGFloat((c >> 24) & 0xff) / 255,
                                green: CGFloat((c >> 16) & 0xff) / 255,
                                blue: CGFloat((c >> 8) & 0xff) / 255,
                                alpha: CGFloat(c & 0xff) / 255)
                colour = c; haveColour = true
            }
            cg.beginPath()
            cg.move(to: CGPoint(x: CGFloat(pts[f * 2]) * side, y: CGFloat(pts[f * 2 + 1]) * side))
            for k in 1..<max(1, n) {
                cg.addLine(to: CGPoint(x: CGFloat(pts[(f + k) * 2]) * side,
                                       y: CGFloat(pts[(f + k) * 2 + 1]) * side))
            }
            cg.closePath()
            cg.fillPath()
        }
    }

    /// The whole board, filled into `cg` now, on this thread.
    public static func fillBoard(active: Int, last: Int, into cg: CGContext, side: CGFloat) {
        fill(boardPolys(active: active, last: last), into: cg, side: side)
    }

    // MARK: motion

    /// Which door a move came through - docs/UI.html's channel grid.
    public enum Channel: Int32 {
        case still = 0, stage = 1, replay = 2, theirs = 3, arrival = 4
        /// B: Send - the post-settlement: only the highlighter moves, to the
        /// outlined block.
        case settle = 6
        /// At rest with my last move staged and unsent: the stage's last
        /// frame, outline and all.
        case draft = 7
        /// A bubble opened: my own replays at my wash's pace, theirs at theirs. The kernel
        /// decides which, from the seat.
        case open = 5
    }

    /// The kernel's plan for the resident game's last move.
    public static func motion(_ ch: Channel) -> UtiMotion { uti_motion(ch.rawValue) }

    /// The board `ms` milliseconds into `plan`. Pure.
    public static func frame(_ plan: UtiMotion, at ms: Int32) -> UtiFrame {
        var p = plan, f = UtiFrame()
        uti_motion_at(&p, ms, &f)
        return f
    }

    /// Every stroke but the last move's mark, and no wash: what is cached.
    public static func underPolys() -> BoardPolys { harvestBoard(uti_draw_under()) }

    /// How far the main lines run past the board, per side, as a fraction of it.
    public static var boardReach: CGFloat { CGFloat(uti_board_reach()) }

    /// Which screen a sheet layout is for (UTTT_SHEET_*).
    public enum SheetKind: Int32 { case play = 0, watch = 1, wait = 2 }

    /// ONE LAYOUT FOR EVERY SCREEN at a drawer height: the board centred on
    /// the sheet and as large as it allows, everything else fitted around.
    /// `words` says the strip carries words (false only for a live seat);
    /// where they go is the layout's `words` box.
    /// `hint`: a bubble waits in the field, so the send hint may stand in
    /// the top right corner and the right column starts under it.
    public static func sheet(_ kind: SheetKind, size: CGSize, words: Bool = true,
                             hint: Bool = false) -> UtiSheet {
        uti_sheet(UtiSheetIn(w: Float(size.width), h: Float(size.height),
                             kind: kind.rawValue, words: words ? 1 : 0, hint: hint ? 1 : 0))
    }

    /// The last move's heavy mark, drawn to `t`: what moves over the cache.
    public static func lastStroke(t: Float) -> [Poly] { harvest(uti_draw_last(t)) }

    /// The last move's settlement - the big mark of the block it won and the
    /// win line of the game it ended - drawn to the frame's `fall_t`, `line_t`.
    /// The promise: the pen outline round `block` in the highlighter's rect
    /// and colour, drawn round to `t`.
    public static func outlineStroke(block: Int32, t: Float) -> [Poly] {
        harvest(uti_draw_outline(block, t))
    }

    public static func settleStroke(fall: Float, line: Float) -> [Poly] {
        harvest(uti_draw_settle(fall, line))
    }

    /// How long nothing moves after a move's whole plan before the drawer
    /// does (UTTT_MS_REST).
    public static var restSeconds: Double { Double(uti_motion_rest_ms()) / 1000 }

    /// One mark, for the "you are" indicator.
    public static func mark(_ m: Mark, seed: Int32) -> [Poly] {
        harvest(uti_draw_mark(Int32(m.rawValue), seed))
    }

    /// The rulebook door. It takes the size the button HAS, in points, because
    /// the kernel's hachure is not scale-free - a bigger button is filled with
    /// more lines rather than the same ones stretched - and it hands back
    /// 0..1 polygons like everything else here.
    public static func rulebook(w: CGFloat, h: CGFloat) -> [Poly] {
        harvest(uti_draw_rulebook(Float(w), Float(h)))
    }

    /// The Again door: a hachured bar in the rulebook's pen, drawn at the
    /// size it has in points. The polygons are 0..1 of the BAR - x over its
    /// width, y over its height - so they fill a rectangle, not a square.
    public static func door(w: CGFloat, h: CGFloat) -> [Poly] {
        harvest(uti_draw_door(Float(w), Float(h)))
    }
}
