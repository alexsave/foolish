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
    }

    public static func say(_ s: Say) -> String { String(cString: uti_say(s.key)) }

    /// The mark the bubble's headline draws before its words, or `.none`.
    public static var bubbleMark: Mark { Mark(rawValue: UInt8(uti_say_bubble_mark())) ?? .none }

    /// The mark drawn inside the play-surface headline, or `.none`.
    public static var sayMark: Mark { Mark(rawValue: UInt8(uti_say_mark())) ?? .none }

    // MARK: the drawing

    /// A polygon the kernel wants filled. Points are 0..1 on both axes.
    public struct Poly {
        public let points: [CGPoint]
        public let color: CGColor
    }

    private static func harvest(_ count: Int32) -> [Poly] {
        guard count > 0,
              let pts = uti_points(), let first = uti_poly_first(),
              let ns = uti_poly_n(), let rgba = uti_poly_rgba() else { return [] }
        var out: [Poly] = []
        out.reserveCapacity(Int(count))
        for i in 0..<Int(count) {
            let f = Int(first[i]), n = Int(ns[i])
            var p: [CGPoint] = []
            p.reserveCapacity(n)
            for k in 0..<n {
                p.append(CGPoint(x: CGFloat(pts[(f + k) * 2]),
                                 y: CGFloat(pts[(f + k) * 2 + 1])))
            }
            let c = rgba[i]
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

    /// THE WHOLE BOARD AS PLAIN VALUES: the kernel's display list copied out
    /// in one pass, so it can be filled anywhere - including off the main
    /// thread, which the kernel itself (one static display list) must never
    /// be touched from. A copy of four flat buffers, not an array per polygon.
    public struct BoardPolys: Sendable {
        let points: [Float]          // x, y pairs, 0..1
        let first: [Int32]
        let count: [Int32]
        let rgba: [UInt32]
    }

    public static func boardPolys(active: Int, last: Int) -> BoardPolys {
        harvestBoard(uti_draw(Int32(active), Int32(last), 1, 1))
    }

    /// The board as the bubble draws it: the main lines stop on its frame.
    public static func bubbleBoardPolys(active: Int, last: Int) -> BoardPolys {
        harvestBoard(uti_draw_bubble(Int32(active), Int32(last)))
    }

    private static func harvestBoard(_ count: Int32) -> BoardPolys {
        let n = Int(count)
        let np = Int(uti_point_count())
        guard n > 0, np > 0, let pts = uti_points(), let first = uti_poly_first(),
              let ns = uti_poly_n(), let rgba = uti_poly_rgba() else {
            return BoardPolys(points: [], first: [], count: [], rgba: [])
        }
        return BoardPolys(points: Array(UnsafeBufferPointer(start: pts, count: np * 2)),
                          first: Array(UnsafeBufferPointer(start: first, count: n)),
                          count: Array(UnsafeBufferPointer(start: ns, count: n)),
                          rgba: Array(UnsafeBufferPointer(start: rgba, count: n)))
    }

    /// Fill `polys` into `cg` with the unit square scaled to `side`: the same
    /// polygons in the same order, one fill each, the colour set only when it
    /// changes. Pure - it reads nothing but its arguments.
    public static func fill(_ polys: BoardPolys, into cg: CGContext, side: CGFloat) {
        var colour: UInt32 = 0
        var haveColour = false
        polys.points.withUnsafeBufferPointer { pts in
            for i in 0..<polys.first.count {
                let f = Int(polys.first[i]), n = Int(polys.count[i])
                guard n > 0, (f + n) * 2 <= pts.count else { continue }
                let c = polys.rgba[i]
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
    }

    /// The whole board, filled into `cg` now, on this thread.
    public static func fillBoard(active: Int, last: Int, into cg: CGContext, side: CGFloat) {
        fill(boardPolys(active: active, last: last), into: cg, side: side)
    }

    /// Only the stroke that is moving. This is what an animation redraws.
    public static func stroke(move: Int, t: Float) -> [Poly] {
        harvest(uti_draw_one(Int32(move), t))
    }

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
}
