// RevealScreen.swift - the call: the kernel's table with every standing cup
// tipped up to show the dice where they lie (DECISIONS I23), the dice that
// count for the called bid ringed in the glow at the places the stage's HUD
// gives (the kernel's `Reveal.counts`, which covers the bid's face and the
// wild 1s), the tally on the plate, the outcome line, the loser's stamp under
// its name, and Next round on the shelf when the kernel offers it. At the end
// the same screen carries the winner and no button.
//
// THE MOTION IS THE KERNEL'S (cn_api_beats_frame, DECISIONS I10): the stage
// lifts the cups by the plan's LIFT beat on the plan's clock, the counting
// dice light one by one as its COUNT beat says, and the loser's stamp and the
// outcome line wait for the plan to finish. Swift holds no duration here.
//
// When the stage draws nothing (a stale pair of readers), the rows below are
// the reveal, as they were before the stage.

import SwiftUI

public struct RevealScreen: View {
    @ObservedObject var host: ChuiniuHost
    @StateObject private var director = StageDirector(stage: KernelSeam.stage())
    @State private var playing = true
    @Environment(\.displayScale) private var displayScale
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
        let start = host.kernel.motionStart
        let t = host.table
        GeometryReader { geo in
            let request = StageRequest(screen: .reveal, drawer: geo.size, scale: displayScale, roll: false,
                                       rollID: t.rollID, table: StageTableKey(t))
            ZStack(alignment: .topLeading) {
                Color.clear
                if let hud = director.hud, let r = t.reveal {
                    // ONCE THE KERNEL SAYS DONE, THE SETTLED REVEAL, not the
                    // timeline's last frame: a paused TimelineView keeps the
                    // frame it last drew, which was seen on the simulator as a
                    // reveal stuck before its outcome line and its Next round
                    if let start, playing {
                        TimelineView(.animation) { ctx in
                            let ms = Int(ctx.date.timeIntervalSince(start) * 1000)
                            overlay(t, r, hud, host.kernel.revealMotion(atMs: ms) ?? Self.settled)
                        }
                    } else {
                        overlay(t, r, hud, Self.settled)
                    }
                } else if director.hud == nil {
                    RevealList(host: host)
                }
            }
            // the table runs on under the safe areas; the HUD keeps to them
            .background(alignment: .topLeading) {
                StageView(director: director, names: names(t, settled: !playing), outWord: host.word(.out),
                          inset: geo.safeAreaInsets)
                    .ignoresSafeArea()
            }
            .onAppear { begin(request) }
            .onChange(of: request) { _, r in begin(r) }
        }
        .background(TableScreen.glassBackground.ignoresSafeArea())
        .task(id: start) {
            // stop sampling once the kernel says the reveal has run
            playing = true
            while let s = start, !Task.isCancelled {
                let ms = Int(Date().timeIntervalSince(s) * 1000)
                if host.kernel.revealMotion(atMs: ms)?.done ?? true { break }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            playing = false
        }
    }

    static let settled = RevealMotion(cupsUp: true, lit: .max, done: true)

    /// The outcome line with the kernel's clauses set apart where the line
    /// says them (the last place each is found): the loser's in blood, the
    /// winner's in the glow. A clause the line does not hold is left alone.
    static func styled(_ line: String, loss: String, win: String) -> AttributedString {
        var a = AttributedString(line)
        for (clause, ink) in [(loss, Ink.blood), (win, Ink.glow)] where !clause.isEmpty {
            if let r = a.range(of: clause, options: .backwards) { a[r].foregroundColor = ink }
        }
        return a
    }

    private func begin(_ r: StageRequest) {
        director.reduceMotion = reduceMotion
        let kernel = host.kernel
        // the cups move while the plan has lit no die yet (the CALL and the
        // LIFT); after that the picture is still
        director.liveAt = { ms in kernel.revealMotion(atMs: ms).map { !$0.done && $0.lit == 0 } ?? false }
        director.begin(r, planMs: reduceMotion ? nil : host.planMs)
    }

    /// The names on the planks; once the reveal has run, the loser's stamp
    /// under its name and the winner's name in brass.
    private func names(_ t: TableModel, settled: Bool) -> [StageName] {
        t.seats.map { seat in
            let loser = settled && t.winner == nil && t.reveal?.loser == seat.id
            return StageName(seat: seat, stamp: loser ? host.word(.loses) : "", won: settled && t.winner == seat.id)
        }
    }

    @ViewBuilder private func overlay(_ t: TableModel, _ r: Reveal, _ hud: CnStageHudSnap, _ motion: RevealMotion) -> some View {
        if motion.cupsUp {
            Rings(reveal: r, hud: hud, lit: motion.lit)
        }
        if motion.done {
            let shelf = hud.shelfRect ?? CGRect(x: 0, y: hud.h - 50, width: hud.w, height: 50)
            // the tally on the plate, the outcome line at the top of its room
            // (the kernel's: under the tall list's last row, over the shelf on
            // a short board), the seats' rows clear of both
            if let p = hud.plateRect { BidPlate(text: r.tally, face: r.bid.face).at(p) }
            let o = hud.outcomeRect ?? CGRect(x: 0, y: max(0, shelf.minY - 54), width: hud.w, height: 48)
            outcome(r)
                .frame(maxWidth: max(0, o.width - 32))
                .fixedSize(horizontal: false, vertical: true)
                .frame(width: o.width, height: o.height, alignment: .top)
                .position(x: o.midX, y: o.midY)
            if t.phase == .revealed, r.nextAllowed {
                // Next round is a step on, not a move: the quiet plate (the study's reveal)
                PlankButton(title: host.word(.nextRound), kind: .quiet, seed: 9) { host.nextRound() }
                    .padding(.horizontal, 16)
                    .frame(width: shelf.width, height: shelf.height, alignment: .top)
                    .position(x: shelf.midX, y: shelf.midY)
            }
        }
    }

    /// The kernel's outcome line (`.t-out`: the roman at 15.5 on the planks),
    /// the loser's clause in blood and at the end the winner's in the glow,
    /// as the study sets them (`.t-out b`).
    private func outcome(_ r: Reveal) -> some View {
        Text(Self.styled(r.outcome, loss: r.outcomeLoss, win: r.outcomeWin))
            .font(FType.serif(15.5))
            .onPlanks()
        .multilineTextAlignment(.center)
        .lineLimit(3)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        // on a `.seatband`'s dark wash, so the line reads over a cup the stage put under it
        .background(SeatBand())
    }
}

/// The counting dice's glow: each counting die's FACE on the glass (the HUD's
/// `die_q`: the die's up face, grown by the study's ring gap, its four
/// corners through the eye and the turn, seat s's die k at (s * 5 + k) * 8,
/// in `Reveal.dice` order), outlined as the study's `die()` outlines a
/// counting die's square: the glow blurred, the glow, a pale core, the
/// corners rounded. Lit one by one in seat order as the kernel's COUNT beat
/// says. The dice that do not count are drowned in the picture itself (the
/// kernel's, by the lift).
struct Rings: View {
    let reveal: Reveal
    let hud: CnStageHudSnap
    let lit: Int

    var body: some View {
        Canvas { ctx, _ in
            var ordinal = 0
            for s in reveal.dice.indices where s < hud.seats {
                let counts = reveal.counts.indices.contains(s) ? reveal.counts[s] : []
                for k in reveal.dice[s].indices where counts.indices.contains(k) && counts[k] {
                    defer { ordinal += 1 }
                    guard ordinal < lit, let q = Self.face(hud, seat: s, die: k) else { continue }
                    let path = Self.rounded(q)
                    // the study's stroke: .07 of the side, never under 1.5, its core .35 of that
                    let side = hud.dieD.indices.contains(s) ? hud.dieD[s] : 10
                    let sw = max(1.5, side * 0.07)
                    ctx.drawLayer { g in
                        g.addFilter(.blur(radius: max(1.5, side * 0.09)))
                        g.stroke(path, with: .color(Ink.glow), lineWidth: sw * 1.6)
                    }
                    ctx.stroke(path, with: .color(Ink.glow), lineWidth: sw)
                    ctx.stroke(path, with: .color(Color(hex: 0xE9FFF7).opacity(0.9)), lineWidth: sw * 0.35)
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    /// Seat s's die k's face on the glass: its four corners in turn, or nil
    /// where the HUD has none.
    static func face(_ hud: CnStageHudSnap, seat s: Int, die k: Int) -> [CGPoint]? {
        let i = (s * Int(CN_STAGE_DICE) + k) * 8
        guard k < Int(CN_STAGE_DICE), hud.dieQ.count >= i + 8 else { return nil }
        let q = (0..<4).map { CGPoint(x: hud.dieQ[i + 2 * $0], y: hud.dieQ[i + 2 * $0 + 1]) }
        return q.allSatisfy({ $0 == .zero }) ? nil : q
    }

    /// A quad with its corners rounded (the study's die square's corners, a
    /// sixth of the shorter side).
    static func rounded(_ q: [CGPoint]) -> Path {
        func mid(_ a: CGPoint, _ b: CGPoint) -> CGPoint { CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2) }
        var short = CGFloat.greatestFiniteMagnitude
        for e in 0..<4 {
            let a = q[e], b = q[(e + 1) % 4]
            short = min(short, hypot(b.x - a.x, b.y - a.y))
        }
        return Path { p in
            p.move(to: mid(q[3], q[0]))
            for c in 0..<4 { p.addArc(tangent1End: q[c], tangent2End: mid(q[c], q[(c + 1) % 4]), radius: short / 6) }
            p.closeSubpath()
        }
    }
}

/// The reveal as rows, settled, for a stage that drew nothing: the study's
/// reveal (`.seatband`s on the planks, the counting dice ringed, the rest
/// drowned, the loser's band edged in blood with its stamp).
struct RevealList: View {
    @ObservedObject var host: ChuiniuHost

    var body: some View {
        let t = host.table
        VStack(spacing: 8) {
            if let r = t.reveal {
                HStack(spacing: 10) {
                    Text(r.tally)
                        .font(FType.serif(26))
                        .bidInk()
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                    Die(face: r.bid.face, size: 30, seed: 80)
                }
                VStack(spacing: 8) {
                    ForEach(t.seats) { seat in
                        RevealRow(seat: seat,
                                  dice: r.dice.indices.contains(seat.id) ? r.dice[seat.id] : [],
                                  counts: r.counts.indices.contains(seat.id) ? r.counts[seat.id] : [],
                                  loser: r.loser == seat.id,
                                  winner: t.winner == seat.id,
                                  losesWord: host.word(.loses))
                    }
                }
                Text(r.outcome)
                    .font(FType.serif(15.5))
                    .onPlanks()
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
            }
            if t.phase == .revealed, t.reveal?.nextAllowed == true {
                PlankButton(title: host.word(.nextRound), kind: .quiet, seed: 9) { host.nextRound() }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.top, 10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(PlanksBackground().ignoresSafeArea())
    }
}

/// One seat at the reveal, settled (`.seatband`): the name in small caps,
/// its dice (the counting ones ringed, the rest drowned), and the loser's
/// stamp.
struct RevealRow: View {
    let seat: SeatModel
    let dice: [Int]
    let counts: [Bool]
    let loser: Bool
    let winner: Bool
    let losesWord: String

    var body: some View {
        HStack(spacing: 6) {
            Text(seat.name)
                .font(FType.sc(14))
                .tracking(FType.nameTracking(14))
                .onPlanks(winner ? Ink.glow : (seat.isMe ? Ink.ink : Ink.inkdim))
                .lineLimit(1)
                .frame(width: 64, alignment: .leading)
                .layoutPriority(1)
            Spacer(minLength: 0)
            HStack(spacing: 4) {
                ForEach(Array(dice.enumerated()), id: \.offset) { i, v in
                    let counted = counts.indices.contains(i) && counts[i]
                    Die(face: v, size: 28, counts: counted, drowned: !counted, seed: 90 + seat.id * 9 + i)
                }
            }
            if loser { Stamp(word: losesWord) }
        }
        .frame(height: 44)
        .padding(.horizontal, 12)
        .background(SeatBand(lost: loser))
        .opacity(seat.alive || !dice.isEmpty ? 1 : 0.5)
    }
}

/// `.seatband`: a dark wash on the planks with a cold hairline on top; edged
/// in blood for the seat that lost.
struct SeatBand: View {
    var lost = false
    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 4, style: .circular)
        shape.fill(Color(.sRGB, red: 2 / 255, green: 10 / 255, blue: 10 / 255, opacity: 0.58))
            .overlay(alignment: .top) { Rectangle().fill(lost ? Ink.blood.opacity(0.18) : Ink.glow.opacity(0.08)).frame(height: 1) }
            .overlay(alignment: .bottom) { Rectangle().fill(Color.black.opacity(0.6)).frame(height: 1) }
            .overlay { if lost { shape.strokeBorder(Ink.blood.opacity(0.28), lineWidth: 1) } }
            .clipShape(shape)
    }
}

/// `.stamp`: a word stamped in blood, capitals tracked wide, turned 5 degrees.
struct Stamp: View {
    let word: String
    var size: CGFloat = 9
    var body: some View {
        Text(word.uppercased())
            .font(FType.sc(size))
            .tracking(size * 0.22)
            .foregroundStyle(Ink.blood)
            .shadow(color: Ink.blood.opacity(0.4), radius: 3)
            .padding(.horizontal, 6).padding(.vertical, 4)
            .background(Color(.sRGB, red: 60 / 255, green: 12 / 255, blue: 8 / 255, opacity: 0.35))
            .overlay(Rectangle().strokeBorder(Color(.sRGB, red: 192 / 255, green: 74 / 255, blue: 51 / 255, opacity: 0.85), lineWidth: 1.5))
            .rotationEffect(.degrees(-5))
            .fixedSize()
    }
}
