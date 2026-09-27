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

    /// foolish's board inset inside the extension's view (the kernel's
    /// coordinates are the board's, so the inset is the kernel's too).
    public static let boardInset = (leading: CGFloat(PK_LAY_INSET_L), trailing: CGFloat(PK_LAY_INSET_R),
                                    top: CGFloat(PK_LAY_INSET_T), bottom: CGFloat(PK_LAY_INSET_B))
    /// The hand's own side padding inside the board.
    public static let handPadding = CGFloat(PK_LAY_HAND_PAD)
    /// A touch that travels less than this is a tap (foolish's tapThreshold).
    public static let tapSlop = CGFloat(PK_LAY_TAP_SLOP)
    public static let pillHeight = CGFloat(PK_LAY_PILL_H)

    /// The board's zones (pk_lay_zone, I31): where a dragged back draws, where
    /// a dragged card plays, the pill row, the toast's centre, the direction box.
    public enum Zone {
        case drawBand, pileDrop, pills, toast, dir
        var c: Int32 {
            switch self {
            case .drawBand: return Int32(PK_ZONE_DRAW_BAND)
            case .pileDrop: return Int32(PK_ZONE_PILE_DROP)
            case .pills:    return Int32(PK_ZONE_PILLS)
            case .toast:    return Int32(PK_ZONE_TOAST)
            case .dir:      return Int32(PK_ZONE_DIR)
            }
        }
    }

    public static func zone(_ z: Zone, board: CGSize, collapse: CGFloat, handBox: CGFloat) -> CGRect {
        var x: Float = 0, y: Float = 0, w: Float = 0, h: Float = 0
        _ = pk_lay_zone(z.c, Float(board.width), Float(board.height), Float(collapse), Float(handBox), &x, &y, &w, &h)
        return CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
    }

    /// What a dragged hand card let go at `point` does (pk_lay_drop, I38): in
    /// the hand row it rearranges and never plays, on the pile it plays.
    public enum Drop: Equatable { case none, hand, pile }

    public static func drop(board: CGSize, collapse: CGFloat, handBox: CGFloat, at point: CGPoint) -> Drop {
        switch pk_lay_drop(Float(board.width), Float(board.height), Float(collapse), Float(handBox),
                           Float(point.x), Float(point.y)) {
        case Int32(PK_DROP_HAND): return .hand
        case Int32(PK_DROP_PILE): return .pile
        default:                  return .none
        }
    }

    /// The slot a dragged card whose centre is at `centre` (the hand box's
    /// coordinates) asks for (pk_lay_hand_nearest, FHandFan.slotIndex).
    public static func handNearest(count n: Int, width: CGFloat, maxRows: Int, centre: CGPoint) -> Int? {
        let s = Int(pk_lay_hand_nearest(Int32(n), Float(width), Int32(maxRows), Float(centre.x), Float(centre.y)))
        return s >= 0 ? s : nil
    }

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

    /// U14: suit tile `tile` (0...3, 4 the x) about the pile's centre.
    public static func pickerTile(_ tile: Int, centre: CGPoint) -> CGPoint {
        var x: Float = 0, y: Float = 0
        pk_lay_picker(Int32(tile), Float(centre.x), Float(centre.y), &x, &y)
        return CGPoint(x: CGFloat(x), y: CGFloat(y))
    }
    public static let pickerTile = CGFloat(PK_LAY_PICKER_TILE)
    public static let pickerX = CGFloat(PK_LAY_PICKER_X)

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
