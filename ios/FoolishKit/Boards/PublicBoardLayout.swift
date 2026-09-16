// PublicBoardLayout.swift - where everything sits on the PUBLIC board
// (MessageBoardView, and through it the 300x195 bubble), as arithmetic with no
// view in it, so a test can ask the one question the bubble kept failing at
// eight players: does it all fit?
//
// THE BUBBLE IS A THUMBNAIL, NOT A SMALLER BOARD.
//
// The ring the live board draws is a 35% ellipse with a full seat badge at
// every point - a name, a fan of card backs and a 40pt role row, 103pt tall.
// On a 300x195 balloon at eight seats that put seven of those on a ring 63pt
// tall: the top seat's name left the picture entirely, the fans lay across
// the battle cards, and the marks piled up along the bottom edge (filmed,
// --twocover 8, September 2026). Nothing was wrong with any one piece; there
// was no arrangement of full-size pieces that fit.
//
// So the public board places SEAT TAGS (FSeatTag, 58x40: a name over one
// landscape card back carrying the count, the role mark beside it) at fixed
// STATIONS round the table's edge, and shrinks the two corner pieces and the
// battle cluster only as far as those tags require. Everything is derived
// from the sizes the components themselves publish, so a component that grows
// a point moves the arithmetic with it rather than silently overlapping.

import CoreGraphics
import Foundation

enum PublicBoardLayout {
    /// The eight places a seat can sit, clockwise from the bottom - the order a
    /// turn comes round the table in, and the same direction the live board's
    /// ring runs (seat 1 is to the LEFT of seat 0).
    enum Station: Int, CaseIterable {
        case bottom = 0, bottomLeft, left, topLeft, top, topRight, right, bottomRight
    }

    /// Which station each seat takes at a table of `n`. Seat 0 is always the
    /// bottom centre (the convention the live board's spectator uses too,
    /// MessageTableView.ringPoint), and the rest are spread evenly round the
    /// eight, each rounded to the nearest. Two seats face each other across
    /// the table, four take the four sides, eight fill every station; the
    /// odd counts skip the stations that would crowd a corner piece.
    static func stations(n: Int) -> [Station] {
        let count = max(n, 1)
        return (0..<count).map { seat in
            let raw = Double(seat) * Double(Station.allCases.count) / Double(count)
            return Station(rawValue: Int(raw.rounded()) % Station.allCases.count)!
        }
    }

    /// One seat tag's footprint (FSeatTag draws inside exactly this box): the
    /// card, 2pt, and the mark box, under a name no wider than that.
    static let tagSize = CGSize(width: 58, height: 40)

    /// How far the outer tags of a top or bottom row sit from the centre line.
    /// 62 leaves 4pt between neighbouring 58pt tags and 51pt in each corner,
    /// which is what the corner pieces need at `scaleFloor`: the discard pile
    /// is 78 x 0.6 = 46.8 wide, and the deck well 74 x 0.6 = 44.4 plus the
    /// 6pt it slides right to clear the balloon's icon (`deckSlide`). Two
    /// earlier values were wrong by a point or two each - 66 on the deck's
    /// number alone put the discard 1.8pt under the top-right tag - and it
    /// was the geometry test, not the picture, that said so.
    static let rowSpread: CGFloat = 62

    static func seatPoint(station: Station, in size: CGSize) -> CGPoint {
        let cx = size.width / 2, cy = size.height / 2
        let hw = tagSize.width / 2, hh = tagSize.height / 2
        switch station {
        case .bottom:      return CGPoint(x: cx, y: size.height - hh)
        case .bottomLeft:  return CGPoint(x: cx - rowSpread, y: size.height - hh)
        case .left:        return CGPoint(x: hw, y: cy)
        case .topLeft:     return CGPoint(x: cx - rowSpread, y: hh)
        case .top:         return CGPoint(x: cx, y: hh)
        case .topRight:    return CGPoint(x: cx + rowSpread, y: hh)
        case .right:       return CGPoint(x: size.width - hw, y: cy)
        case .bottomRight: return CGPoint(x: cx + rowSpread, y: size.height - hh)
        }
    }

    static func seatPoint(seat: Int, n: Int, in size: CGSize) -> CGPoint {
        let list = stations(n: n)
        let station = seat >= 0 && seat < list.count ? list[seat] : .bottom
        return seatPoint(station: station, in: size)
    }

    static func seatRect(seat: Int, n: Int, in size: CGSize) -> CGRect {
        let c = seatPoint(seat: seat, n: n, in: size)
        return CGRect(x: c.x - tagSize.width / 2, y: c.y - tagSize.height / 2,
                      width: tagSize.width, height: tagSize.height)
    }

    static func seatRects(n: Int, in size: CGSize) -> [CGRect] {
        (0..<max(n, 0)).map { seatRect(seat: $0, n: n, in: size) }
    }

    // MARK: Corners

    /// How small the deck well and the discard pile may go. At 0.6 the stock's
    /// cards are 28x40 - the size the old mini-fan drew a card back at, which
    /// the owner had already accepted as legible on this surface.
    static let scaleFloor: CGFloat = 0.6

    /// THE BALLOON'S OWN ICON. Messages draws the iMessage app's roundel over
    /// the top-left corner of every balloon, on top of whatever the picture
    /// has there: a circle about 28pt across, 6pt in from the bubble's edge -
    /// measured off a transcript frame, so in this board's coordinates (8pt
    /// inside the bubble) its centre is (11, 11). The full-size deck well
    /// keeps its count clear of it by accident of size, at (41, 31); a well
    /// at `scaleFloor` puts its count at (25, 19), squarely under it, and the
    /// first eight-seat frame shot on the simulator had no deck count at all.
    static let balloonIconCentre = CGPoint(x: 11, y: 11)
    static let balloonIconRadius: CGFloat = 14

    /// How far the deck well slides, right and down, at `scale`: nothing at
    /// full size, (6, 12) at the floor - enough to carry the count out from
    /// under the balloon's icon (`deckCountCentre`) and no more, because the
    /// top-left seat tag starts 51pt in and the left one 69.5pt down.
    static func deckSlide(scale: CGFloat) -> CGPoint {
        CGPoint(x: 15 * (1 - scale), y: 30 * (1 - scale))
    }

    /// The deck well's ink at `scale`, scaled about the board's top-left
    /// corner (where FDeckWell anchors every state) and then slid.
    static func deckRect(scale: CGFloat) -> CGRect {
        let slide = deckSlide(scale: scale)
        return CGRect(x: slide.x, y: slide.y,
                      width: FDeckWell.inkFootprint.width * scale,
                      height: FDeckWell.inkFootprint.height * scale)
    }

    /// Where the deck's count chip lands at `scale`, for the icon check.
    static func deckCountCentre(scale: CGFloat) -> CGPoint {
        let slide = deckSlide(scale: scale)
        return CGPoint(x: FDeckWell.countCentre.x * scale + slide.x,
                       y: FDeckWell.countCentre.y * scale + slide.y)
    }

    /// The discard pile's box, scaled about the top-right corner; the -3 is
    /// the offset MessageBoardView gives it (see the note there).
    static let discardLift: CGFloat = -3
    static func discardRect(scale: CGFloat, in size: CGSize) -> CGRect {
        let w = FDiscardPile.footprint.width * scale
        let h = FDiscardPile.footprint.height * scale
        return CGRect(x: size.width - w, y: discardLift, width: w, height: h)
    }

    /// The largest scale, from 1 down in steps of 0.05, at which neither
    /// corner piece touches a seat tag; the floor if none does. Both corners
    /// share one number so they stay a pair.
    static func cornerScale(n: Int, in size: CGSize) -> CGFloat {
        let seats = seatRects(n: n, in: size)
        return fit(floor: scaleFloor) { s in
            let d = deckRect(scale: s), p = discardRect(scale: s, in: size)
            return !seats.contains { $0.intersects(d) || $0.intersects(p) }
        }
    }

    // MARK: Battles

    /// The battle cluster may go smaller than the corners: at 0.5 a card is
    /// 25x35, which still shows its rank and suit at bubble size, and six
    /// battles at eight seats have nowhere else to go.
    static let gridFloor: CGFloat = 0.5

    static func gridRect(pairs: Int, scale: CGFloat, in size: CGSize) -> CGRect {
        let natural = FBattleGrid.naturalSize(pairs: pairs)
        let w = natural.width * scale, h = natural.height * scale
        return CGRect(x: (size.width - w) / 2, y: (size.height - h) / 2, width: w, height: h)
    }

    /// The largest scale at which the centred cluster of `pairs` battles
    /// clears every seat tag AND both corner pieces (at their own scale). The
    /// corners are settled first: they are the frame of the picture, and a
    /// deck well that changed size from one bubble to the next as the table
    /// filled would read as a different board.
    static func gridScale(n: Int, pairs: Int, in size: CGSize) -> CGFloat {
        guard pairs > 0 else { return 1 }
        let corner = cornerScale(n: n, in: size)
        let blockers = seatRects(n: n, in: size)
            + [deckRect(scale: corner), discardRect(scale: corner, in: size)]
        return fit(floor: gridFloor) { s in
            let g = gridRect(pairs: pairs, scale: s, in: size)
            return !blockers.contains { $0.intersects(g) }
        }
    }

    /// Step down from 1 by 0.05 until `fits`; `floor` if nothing does.
    static func fit(floor: CGFloat, _ fits: (CGFloat) -> Bool) -> CGFloat {
        var s: CGFloat = 1
        while s > floor + 0.0001 {
            if fits(s) { return s }
            s -= 0.05
        }
        return floor
    }
}
