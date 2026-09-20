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
