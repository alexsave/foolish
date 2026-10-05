// RevealScreen.swift - the call: the kernel's table with every standing cup
// tipped up to show the dice where they lie (DECISIONS I23), the dice that
// count for the called bid ringed in brass at the places the stage's HUD
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
            if let p = hud.plateRect, hud.shortBoard == 0 {
                // a tall board: the tally and the outcome together at the
                // plate's place, as wide as the glass (my name sits just over
                // the shelf, where a line there would cover it)
                VStack(spacing: 2) {
                    HStack(spacing: 8) {
                        Text(r.tally).font(.system(size: 20, weight: .heavy)).onFeltText()
                            .lineLimit(1).minimumScaleFactor(0.5)
                        Die(face: r.bid.face, size: 22)
                    }
                    outcome(t, r)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.black.opacity(0.55)))
                .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(FColor.win.opacity(0.55), lineWidth: 1))
                .frame(maxWidth: max(0, hud.w - 32))
                .fixedSize(horizontal: false, vertical: true)
                .frame(width: hud.w, height: 200, alignment: .top)
                .position(x: hud.w / 2, y: p.minY + 100)
            } else {
                if let p = hud.plateRect { BidPlate(text: r.tally, face: r.bid.face).at(p) }
                let above = max(0, shelf.minY - 6)
                outcome(t, r)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.black.opacity(0.55)))
                    .frame(maxWidth: max(0, hud.w - 32))
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(width: hud.w, height: above, alignment: .bottom)
                    .position(x: hud.w / 2, y: above / 2)
            }
            if t.phase == .revealed, r.nextAllowed {
                WoodButton(title: host.word(.nextRound), height: 40, fontSize: 16) { host.nextRound() }
                    .padding(.horizontal, 16)
                    .frame(width: shelf.width, height: shelf.height, alignment: .top)
                    .position(x: shelf.midX, y: shelf.midY)
            }
        }
    }

    /// The kernel's outcome line, and at the end its headline ("You win").
    private func outcome(_ t: TableModel, _ r: Reveal) -> some View {
        VStack(spacing: 4) {
                Text(r.outcome)
                    .font(.system(size: 14, weight: .heavy))
                    .onFeltText()
                if !t.caption.isEmpty, t.phase == .over {
                    Text(t.caption)
                        .font(.system(size: 12, weight: .semibold))
                        .onFeltText(FColor.textDim)
                }
            }
        .multilineTextAlignment(.center)
        .lineLimit(3)
    }
}

/// The counting dice's brass rings, at each shown die's place on the glass
/// (the HUD's `die_x`, `die_y`: seat s at s * stride, in `Reveal.dice` order),
/// lit one by one in seat order as the kernel's COUNT beat says.
struct Rings: View {
    let reveal: Reveal
    let hud: CnStageHudSnap
    let lit: Int

    var body: some View {
        let stride = hud.cupX.isEmpty ? 0 : hud.dieX.count / hud.cupX.count
        Canvas { ctx, _ in
            guard stride > 0 else { return }
            var ordinal = 0
            for s in reveal.dice.indices where s < hud.seats {
                let counts = reveal.counts.indices.contains(s) ? reveal.counts[s] : []
                let pts = reveal.dice[s].indices.compactMap { k -> CGPoint? in
                    let i = s * stride + k
                    guard k < stride, hud.dieX.indices.contains(i), hud.dieX[i] != 0 || hud.dieY[i] != 0 else { return nil }
                    return CGPoint(x: hud.dieX[i], y: hud.dieY[i])
                }
                let r = Self.radius(pts, fallback: (s == hud.me ? hud.myR : hud.cupR) * 0.3)
                for (k, p) in pts.enumerated() where counts.indices.contains(k) && counts[k] {
                    defer { ordinal += 1 }
                    guard ordinal < lit else { continue }
                    let ring = Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r))
                    ctx.stroke(ring, with: .color(FColor.win.opacity(0.45)), lineWidth: 5)
                    ctx.stroke(ring, with: .color(FColor.win), lineWidth: 2)
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    /// A ring round a die: a little under half the nearest neighbour's
    /// distance, so two rings never cross (a painting choice, no game number).
    static func radius(_ pts: [CGPoint], fallback: Double) -> Double {
        var best = Double.infinity
        for i in pts.indices {
            for j in pts.indices where j > i {
                best = min(best, hypot(pts[i].x - pts[j].x, pts[i].y - pts[j].y))
            }
        }
        return best.isFinite ? best * 0.47 : fallback
    }
}

/// The reveal as rows, settled, for a stage that drew nothing.
struct RevealList: View {
    @ObservedObject var host: ChuiniuHost

    var body: some View {
        let t = host.table
        VStack(spacing: 8) {
            if let r = t.reveal {
                HStack(spacing: 10) {
                    Text(r.tally)
                        .font(.system(size: 22, weight: .heavy))
                        .onFeltText()
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                    Die(face: r.bid.face, size: 26)
                }
                VStack(spacing: 6) {
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
                    .font(.system(size: 14, weight: .heavy))
                    .onFeltText()
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
            }
            Text(t.caption)
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(FColor.textDim)
                .multilineTextAlignment(.center)
                .lineLimit(2)
            if t.phase == .revealed, t.reveal?.nextAllowed == true {
                WoodButton(title: host.word(.nextRound), height: 44, fontSize: 16) { host.nextRound() }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.top, 10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}

/// One seat at the reveal, settled: name, its dice (the counting ones ringed,
/// the rest dimmed), and the loser's stamp.
struct RevealRow: View {
    let seat: SeatModel
    let dice: [Int]
    let counts: [Bool]
    let loser: Bool
    let winner: Bool
    let losesWord: String

    var body: some View {
        HStack(spacing: 8) {
            Text(seat.name)
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(winner ? FColor.win : FColor.textPrimary)
                .lineLimit(1)
                .frame(width: 84, alignment: .leading)
                // the name keeps its size when the loser's stamp takes room
                .layoutPriority(1)
            HStack(spacing: 6) {
                ForEach(Array(dice.enumerated()), id: \.offset) { i, v in
                    let counted = counts.indices.contains(i) && counts[i]
                    Die(face: v, size: 28, highlight: counted, dimmed: !counted)
                }
            }
            Spacer(minLength: 0)
            if loser {
                Text(losesWord)
                    .font(.system(size: 11, weight: .heavy))
                    .foregroundColor(.white)
                    .padding(.horizontal, 8).padding(.vertical, 5)
                    .background(RoundedRectangle(cornerRadius: 8).fill(FColor.red))
                    .fixedSize()
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.black.opacity(loser ? 0.28 : 0.14)))
        .opacity(seat.alive || !dice.isEmpty ? 1 : 0.5)
    }
}
