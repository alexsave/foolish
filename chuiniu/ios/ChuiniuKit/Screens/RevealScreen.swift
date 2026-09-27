// RevealScreen.swift - the call: every seat's dice face up, the dice that
// count for the called bid ringed in brass (the kernel's `Reveal.counts`,
// which covers the bid's face and the wild 1s), the rest dimmed, the loser
// marked, and Next round when the kernel offers it. At the end the same
// screen carries the winner in the caption and no button.

import SwiftUI

public struct RevealScreen: View {
    @ObservedObject var host: ChuiniuHost

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
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
            }
            Text(t.caption)
                .font(.system(size: 14, weight: .semibold))
                .onFeltText()
                .multilineTextAlignment(.center)
                .lineLimit(2)
            if t.phase == .revealed, t.reveal?.nextAllowed == true {
                WoodButton(title: host.word(.nextRound), height: 48, fontSize: 16) { host.nextRound() }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.top, 16)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}

/// One seat at the reveal: name, its dice, and the loser's stamp.
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
                .minimumScaleFactor(0.7)
                .frame(width: 84, alignment: .leading)
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
