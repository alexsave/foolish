// DiceTable.swift - the felt table: up to six seats, each a cup with its dice
// count, a name and a turn marker; my own dice face up in a row at the
// bottom; the bid on the table large in the middle ("four 3s").
//
// Everything drawn is a field of the model. Where things go is
// `DiceTableLayout`, a pure function of the seat count, my seat and the
// board's size, so the tests can check it without drawing.

import SwiftUI

/// Where every seat and the bid plate sit on a board.
///
/// TWO SHAPES. A tall board (the expanded drawer) puts the other seats round
/// the upper half of an ellipse with the plate between. A SHORT board (the
/// compact drawer, where the picker leaves about 150pt) cannot hold a badge,
/// the plate and my band stacked, so the other seats go left to right in one
/// row along the top, the plate takes the rest of that row, and my band is
/// only as tall as my dice and name.
public enum DiceTableLayout {
    /// One other seat's badge: name, cup, turn marker.
    public static let badge = CGSize(width: 72, height: 72)
    /// The band at the bottom that holds my name and my dice.
    public static let myBandHeight: CGFloat = 120
    /// The same on a short board: one row of dice, the name, the turn bar.
    public static let shortBandHeight: CGFloat = 64
    /// The bid plate in the middle.
    public static let plate = CGSize(width: 160, height: 56)
    /// The narrowest a plate may be squeezed to beside a row of seats.
    public static let plateMinWidth: CGFloat = 100
    static let margin: CGFloat = 8

    /// The seat drawn at the bottom: mine, or seat 0 for a spectator.
    public static func bottomSeat(me: Int?) -> Int { me ?? 0 }

    /// A board too short for the ellipse.
    public static func isShort(_ board: CGSize) -> Bool {
        board.height < badge.height + plate.height + myBandHeight + 4 * margin
    }

    public static func bandHeight(_ board: CGSize) -> CGFloat { isShort(board) ? shortBandHeight : myBandHeight }

    /// The frame of every seat, indexed by seat. The bottom seat takes the
    /// band at the bottom; the others go left to right in seat order starting
    /// after the bottom seat, round the upper half of an ellipse on a tall
    /// board and in one top row on a short one.
    public static func seatFrames(count: Int, me: Int?, board: CGSize) -> [CGRect] {
        guard count > 0 else { return [] }
        let bottom = bottomSeat(me: me)
        let band = bandHeight(board)
        let bandTop = board.height - band
        var frames = [CGRect](repeating: .zero, count: count)
        frames[bottom] = CGRect(x: margin, y: bandTop, width: board.width - 2 * margin, height: band)
        let others = count - 1
        guard others > 0 else { return frames }
        if isShort(board) {
            let step = min(badge.width + margin, (board.width - 2 * margin) / CGFloat(others))
            let w = min(badge.width, step)
            let y = max(0, (bandTop - badge.height) / 2)
            for j in 0..<others {
                let seat = (bottom + 1 + j) % count
                frames[seat] = CGRect(x: margin + CGFloat(j) * step, y: y, width: w, height: badge.height)
            }
            return frames
        }
        let c = arcCentre(board)
        let (rx, ry) = radii(board)
        for j in 0..<others {
            let seat = (bottom + 1 + j) % count
            let theta = Double.pi - (Double(j) + 0.5) * Double.pi / Double(others)
            let x = c.x + rx * CGFloat(cos(theta))
            let y = c.y - ry * CGFloat(sin(theta))
            frames[seat] = CGRect(x: x - badge.width / 2, y: y - badge.height / 2,
                                  width: badge.width, height: badge.height)
        }
        return frames
    }

    /// The bid plate's frame, or nil on a short board whose row of seats
    /// leaves it no room (five or more other seats).
    public static func plateFrame(count: Int, board: CGSize) -> CGRect? {
        if isShort(board) {
            let others = CGFloat(max(count - 1, 0))
            let rowEnd = margin + others * (badge.width + margin)
            let w = min(plate.width, board.width - margin - rowEnd)
            guard w >= plateMinWidth else { return nil }
            let bandTop = board.height - shortBandHeight
            let y = max(0, (bandTop - plate.height) / 2)
            return CGRect(x: board.width - margin - w, y: y, width: w, height: plate.height)
        }
        let c = arcCentre(board)
        let (_, ry) = radii(board)
        let y = c.y - ry * 0.3
        return CGRect(x: board.width / 2 - plate.width / 2, y: y - plate.height / 2,
                      width: plate.width, height: plate.height)
    }

    static func arcCentre(_ board: CGSize) -> CGPoint {
        CGPoint(x: board.width / 2, y: board.height - myBandHeight - margin)
    }

    static func radii(_ board: CGSize) -> (CGFloat, CGFloat) {
        let c = arcCentre(board)
        let rx = (board.width - badge.width) / 2 - margin
        let ry = c.y - badge.height / 2 - margin
        return (max(rx, 0), max(ry, 0))
    }
}

public struct DiceTable: View {
    public let table: TableModel
    public let outWord: String
    /// The newest roll this phone has played (the host keeps it).
    public let played: Int
    public let onPlayed: (Int) -> Void

    public init(table: TableModel, outWord: String, played: Int,
                onPlayed: @escaping (Int) -> Void) {
        self.table = table
        self.outWord = outWord
        self.played = played
        self.onPlayed = onPlayed
    }

    public var body: some View {
        GeometryReader { geo in
            let board = geo.size
            let frames = DiceTableLayout.seatFrames(count: table.seats.count, me: table.me, board: board)
            let bottom = DiceTableLayout.bottomSeat(me: table.me)
            ZStack(alignment: .topLeading) {
                Color.clear
                if !table.bidText.isEmpty, let p = DiceTableLayout.plateFrame(count: table.seats.count, board: board) {
                    BidPlate(text: table.bidText, face: table.bid?.face)
                        .frame(width: p.width, height: p.height)
                        .position(x: p.midX, y: p.midY)
                }
                ForEach(table.seats) { seat in
                    if frames.indices.contains(seat.id) {
                        let f = frames[seat.id]
                        Group {
                            if seat.id == bottom {
                                MyBand(seat: seat, dice: table.myDice, rollID: table.rollID, played: played,
                                       onPlayed: onPlayed)
                            } else {
                                SeatCup(seat: seat, outWord: outWord)
                            }
                        }
                        .frame(width: f.width, height: f.height)
                        .position(x: f.midX, y: f.midY)
                    }
                }
            }
        }
    }
}

/// The bid in the middle: the kernel's words, big, beside the face.
struct BidPlate: View {
    let text: String
    let face: Int?

    var body: some View {
        HStack(spacing: 10) {
            Text(text)
                .font(.system(size: 30, weight: .heavy))
                .onFeltText()
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            if let face { Die(face: face, size: 30) }
        }
    }
}

/// Another seat: its name, its cup with the dice count on it, and the
/// brass turn bar (foolish's U5).
struct SeatCup: View {
    let seat: SeatModel
    let outWord: String

    var body: some View {
        VStack(spacing: 3) {
            Text(seat.name)
                .font(.system(size: 12, weight: .semibold))
                .onFeltText(seat.isTurn ? FColor.win : FColor.textPrimary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            ZStack(alignment: .bottomTrailing) {
                Cup(width: 34)
                if seat.alive {
                    CountBadge(n: seat.dice).offset(x: 8, y: 4)
                } else {
                    Text(outWord)
                        .font(.system(size: 10, weight: .heavy))
                        .foregroundColor(Color(hex: 0x241804))
                        .padding(.horizontal, 5).padding(.vertical, 2)
                        .background(Capsule().fill(FColor.win))
                        .offset(x: 12, y: 4)
                }
            }
            .opacity(seat.alive ? 1 : 0.5)
            TurnBar(on: seat.isTurn).frame(width: 44)
        }
    }
}

/// The dice count on a cup: a bone disc with the number.
struct CountBadge: View {
    let n: Int
    var body: some View {
        Text(verbatim: "\(n)")
            .font(.system(size: 12, weight: .heavy).monospacedDigit())
            .foregroundColor(FColor.ink)
            .frame(width: 20, height: 20)
            .background(Circle().fill(FColor.card))
            .overlay(Circle().strokeBorder(Color.black.opacity(0.3), lineWidth: 1))
            .shadow(color: .black.opacity(0.4), radius: 1.5, y: 1)
    }
}

struct TurnBar: View {
    let on: Bool
    var body: some View {
        RoundedRectangle(cornerRadius: 2).fill(FColor.win)
            .frame(height: 3)
            .shadow(color: FColor.win.opacity(0.7), radius: 4)
            .opacity(on ? 1 : 0)
            .animation(FMotion.chrome, value: on)
    }
}

/// My band: my name with the turn bar, and my dice face up, rolled out of
/// the cup when a new round's dice arrive.
struct MyBand: View {
    let seat: SeatModel
    let dice: [Int]
    let rollID: Int
    let played: Int
    let onPlayed: (Int) -> Void

    var body: some View {
        VStack(spacing: 4) {
            Spacer(minLength: 0)
            if dice.isEmpty {
                SeatCup(seat: seat, outWord: "")
            } else {
                DiceRoll(values: dice, rollID: rollID, played: played, dieSize: 36, onPlayed: onPlayed)
                HStack(spacing: 6) {
                    Text(seat.name)
                        .font(.system(size: 12, weight: .semibold))
                        .onFeltText(seat.isTurn ? FColor.win : FColor.textPrimary)
                        .lineLimit(1)
                }
                TurnBar(on: seat.isTurn).frame(width: 60)
            }
        }
    }
}
