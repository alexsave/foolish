// PkLayout.swift - the layout numbers, read from C (pickemup/c/ios/pk_lay.c).
//
// Nothing here derives a number: each value is one call into the kernel's
// layout, which is where UI.html's thresholds (O4, U2, U3, U6, U7, U9) live
// and are tested. This file only turns C's floats into CGFloat and names.

import CoreGraphics
import CPickemup

public enum PkLayout {

    public static let cardH = CGFloat(PK_LAY_CARD_H)
    public static let thinBelow = CGFloat(PK_LAY_THIN_W)
    public static let pileSize = CGSize(width: CGFloat(PK_LAY_PILE_W), height: CGFloat(PK_LAY_PILE_H))
    public static let deckSize = CGSize(width: CGFloat(PK_LAY_DECK_W), height: CGFloat(PK_LAY_DECK_H))
    public static let fanCard = CGSize(width: CGFloat(PK_LAY_FAN_CARD_W), height: CGFloat(PK_LAY_FAN_CARD_H))

    /// foolish's board inset inside the extension's view: 8 / 8 / 14 / 4.
    public static let boardInset = (leading: CGFloat(8), trailing: CGFloat(8), top: CGFloat(14), bottom: CGFloat(4))
    /// The hand's own side padding inside the board.
    public static let handPadding: CGFloat = 8

    public static func collapse(viewHeight: CGFloat) -> CGFloat { CGFloat(pk_lay_collapse(Float(viewHeight))) }
    public static func maxRows(viewHeight: CGFloat) -> Int { Int(pk_lay_max_rows(Float(viewHeight))) }

    public enum HandMode: Equatable { case flat, overlap, scroll }

    /// One hand's layout: every card's frame in the hand's box.
    public struct Hand: Equatable {
        public let mode: HandMode
        public let cardW: CGFloat
        public let step: CGFloat
        public let rows: Int
        public let topCount: Int
        public let contentWidth: CGFloat
        public let boxHeight: CGFloat
        public let slots: [CGRect]

        /// A flat card under the thin threshold drops its corners (FCard's rule);
        /// an overlapped card keeps a full 40pt face (U8).
        public var thin: Bool { mode == .flat && cardW < PkLayout.thinBelow }
    }

    public static func hand(count n: Int, width: CGFloat, maxRows: Int) -> Hand {
        var cw: Float = 0, step: Float = 0, content: Float = 0, box: Float = 0
        var rows: Int32 = 0, top: Int32 = 0
        let mode = pk_lay_hand(Int32(n), Float(width), Int32(maxRows), &cw, &step, &rows, &top, &content, &box)
        var slots: [CGRect] = []
        slots.reserveCapacity(max(n, 0))
        for i in 0..<max(n, 0) {
            var x: Float = 0, y: Float = 0
            _ = pk_lay_hand_slot(Int32(n), Float(width), Int32(maxRows), Int32(i), &x, &y)
            slots.append(CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(cw), height: cardH))
        }
        let m: HandMode = mode == Int32(PK_LAY_SCROLL) ? .scroll : mode == Int32(PK_LAY_OVERLAP) ? .overlap : .flat
        return Hand(mode: m, cardW: CGFloat(cw), step: CGFloat(step), rows: Int(rows), topCount: Int(top),
                    contentWidth: CGFloat(content), boxHeight: CGFloat(box), slots: slots)
    }

    /// A seat badge's centre on foolish's ring, my own seat at the bottom.
    public static func seat(_ seat: Int, me: Int, count n: Int, board: CGSize, collapse: CGFloat) -> CGPoint {
        var x: Float = 0, y: Float = 0
        pk_lay_seat(Int32(seat), Int32(me), Int32(n), Float(board.width), Float(board.height), Float(collapse), &x, &y)
        return CGPoint(x: CGFloat(x), y: CGFloat(y))
    }

    public static func fanStep(backs: Int) -> CGFloat { CGFloat(pk_lay_fan_step(Int32(backs))) }
    public static func deckLayers(_ deckCount: Int) -> Int { Int(pk_lay_deck_layers(Int32(deckCount))) }

    public static func pileCentre(board: CGSize, collapse: CGFloat) -> CGPoint {
        var x: Float = 0, y: Float = 0
        pk_lay_pile(Float(board.width), Float(board.height), Float(collapse), &x, &y)
        return CGPoint(x: CGFloat(x), y: CGFloat(y))
    }

    public static func deckOrigin(board: CGSize, collapse: CGFloat) -> CGPoint {
        var x: Float = 0, y: Float = 0
        pk_lay_deck(Float(board.width), Float(board.height), Float(collapse), &x, &y)
        return CGPoint(x: CGFloat(x), y: CGFloat(y))
    }

    public enum Pill: Equatable { case none, draw, play, pass, undo }

    /// U9: what stands in the trailing slot and the one to its left.
    public static func pills(canDraw: Bool, myTurn: Bool, selected: Bool, canPass: Bool,
                             canUndo: Bool) -> (trailing: Pill, leading: Pill) {
        var t: Int32 = 0, l: Int32 = 0
        pk_lay_pills(canDraw ? 1 : 0, myTurn ? 1 : 0, selected ? 1 : 0, canPass ? 1 : 0, canUndo ? 1 : 0, &t, &l)
        return (pill(t), pill(l))
    }

    private static func pill(_ v: Int32) -> Pill {
        switch v {
        case Int32(PK_PILL_DRAW): return .draw
        case Int32(PK_PILL_PLAY): return .play
        case Int32(PK_PILL_PASS): return .pass
        case Int32(PK_PILL_UNDO): return .undo
        default:                  return .none
        }
    }
}
