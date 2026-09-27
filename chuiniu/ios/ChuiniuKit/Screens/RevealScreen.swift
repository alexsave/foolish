// RevealScreen.swift - the call: every seat's dice face up, the dice that
// count for the called bid ringed in brass (the kernel's `Reveal.counts`,
// which covers the bid's face and the wild 1s), the rest dimmed, the loser
// marked, and Next round when the kernel offers it. At the end the same
// screen carries the winner in the caption and no button.
//
// THE MOTION IS THE KERNEL'S (cn_api_beats_frame, DECISIONS I10): while a
// call's plan plays, the cups stay down until its LIFT beat, the counting
// dice light one by one as its COUNT beat says, and the loser's stamp and the
// outcome line wait for the plan to finish. Swift holds no duration here.

import SwiftUI

public struct RevealScreen: View {
    @ObservedObject var host: ChuiniuHost
    @State private var playing = true

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
        let start = host.kernel.motionStart
        Group {
            // ONCE THE KERNEL SAYS DONE, THE SETTLED REVEAL, not the timeline's
            // last frame: a paused TimelineView keeps the frame it last drew,
            // which was seen on the simulator as a reveal stuck before its
            // outcome line and its Next round
            if let start, playing {
                TimelineView(.animation) { ctx in
                    let ms = Int(ctx.date.timeIntervalSince(start) * 1000)
                    content(host.kernel.revealMotion(atMs: ms) ?? Self.settled)
                }
            } else {
                content(Self.settled)
            }
        }
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

    private static let settled = RevealMotion(cupsUp: true, lit: .max, done: true)

    @ViewBuilder private func content(_ motion: RevealMotion) -> some View {
        let t = host.table
        VStack(spacing: 12) {
            if let r = t.reveal {
                HStack(spacing: 10) {
                    Text(r.tally)
                        .font(.system(size: 22, weight: .heavy))
                        .onFeltText()
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                    Die(face: r.bid.face, size: 26)
                }
                .opacity(motion.done ? 1 : 0)
                VStack(spacing: 8) {
                    ForEach(t.seats) { seat in
                        RevealRow(seat: seat,
                                  dice: r.dice.indices.contains(seat.id) ? r.dice[seat.id] : [],
                                  counts: r.counts.indices.contains(seat.id) ? r.counts[seat.id] : [],
                                  litBefore: litBefore(r, seat.id),
                                  motion: motion,
                                  loser: r.loser == seat.id,
                                  winner: t.winner == seat.id,
                                  losesWord: host.word(.loses))
                    }
                }
                if motion.done {
                    Text(r.outcome)
                        .font(.system(size: 14, weight: .heavy))
                        .onFeltText()
                        .multilineTextAlignment(.center)
                        .lineLimit(3)
                }
            }
            Text(t.caption)
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(FColor.textDim)
                .multilineTextAlignment(.center)
                .lineLimit(2)
            if motion.done, t.phase == .revealed, t.reveal?.nextAllowed == true {
                WoodButton(title: host.word(.nextRound), height: 48, fontSize: 16) { host.nextRound() }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.top, 16)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }

    /// How many counting dice the seats before `seat` hold: where this row's
    /// dice fall in the kernel's one-by-one count (seat order). A position,
    /// not a tally: which dice count is the kernel's `counts`.
    private func litBefore(_ r: Reveal, _ seat: Int) -> Int {
        r.counts.prefix(max(0, min(seat, r.counts.count))).reduce(0) { $0 + $1.filter { $0 }.count }
    }
}

/// One seat at the reveal: name, its dice (or its cup while the cups are
/// down), and the loser's stamp.
struct RevealRow: View {
    let seat: SeatModel
    let dice: [Int]
    let counts: [Bool]
    let litBefore: Int
    let motion: RevealMotion
    let loser: Bool
    let winner: Bool
    let losesWord: String

    var body: some View {
        HStack(spacing: 8) {
            Text(seat.name)
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(winner ? FColor.win : FColor.textPrimary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .frame(width: 84, alignment: .leading)
                // the name keeps its size when the loser's stamp takes room
                .layoutPriority(1)
            if motion.cupsUp {
                HStack(spacing: 6) {
                    ForEach(Array(dice.enumerated()), id: \.offset) { i, v in
                        let counted = counts.indices.contains(i) && counts[i]
                        let ordinal = litBefore + counts.prefix(i).filter { $0 }.count
                        let lit = counted && ordinal < motion.lit
                        Die(face: v, size: 28, highlight: lit, dimmed: motion.done && !counted)
                    }
                }
                .transition(.opacity)
            } else if !dice.isEmpty {
                Cup(width: 30)
            }
            Spacer(minLength: 0)
            if loser && motion.done {
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
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.black.opacity(loser && motion.done ? 0.28 : 0.14)))
        .opacity(seat.alive || !dice.isEmpty ? 1 : 0.5)
        .animation(FMotion.chrome, value: motion.cupsUp)
    }
}
