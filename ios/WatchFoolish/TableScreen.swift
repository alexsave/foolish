// TableScreen.swift — the glance screen (docs/WATCHOS_APP_PLAN.md §5.1). The four
// corners hold deck (TL) · discard (TR) · flipped trump (BL) · sword→Action (BR);
// the seat ring (you at 6 o'clock, names arced along the ring) sits between them,
// and the hero battle — ONE attack/cover pair, Crown pages — is the center.

import SwiftUI

struct TableScreen: View {
    @ObservedObject var game: MockGame
    var gameId: String = "g1"
    let onPlay: () -> Void

    @State private var crown: Double = 0
    private var focus: Int { min(max(Int(crown.rounded()), 0), max(game.battles.count - 1, 0)) }

    private struct SeatVM: Identifiable {
        let id: Int; let name: String; let count: Int
        let isSelf: Bool; let isDefender: Bool; let isAttacker: Bool; let isOut: Bool
    }
    private var seats: [SeatVM] {
        var out = [SeatVM(id: -1, name: "You", count: game.hand.count, isSelf: true,
                          isDefender: game.defenderSeat == -1, isAttacker: game.attackerSeat == -1, isOut: false)]
        for o in game.opponents {
            out.append(SeatVM(id: o.id, name: o.name, count: o.handCount, isSelf: false,
                              isDefender: game.defenderSeat == o.id, isAttacker: game.attackerSeat == o.id, isOut: o.isOut))
        }
        return out
    }

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            let center = CGPoint(x: w / 2, y: h * 0.42)
            let r = 0.33 * min(w, h)
            ZStack {
                ForEach(Array(seats.enumerated()), id: \.element.id) { i, s in
                    seat(s).position(pos(i, n: seats.count, center: center, r: r))
                }
                RingNames(items: seats.enumerated().map { i, s in
                    RingNames.Item(id: s.id, angle: angle(i, n: seats.count), name: s.name,
                                   color: s.isSelf ? WColor.brass.opacity(0.9) : WColor.dim)
                }, radius: r + 9, fontSize: 8.5, step: 0.13)
                .position(center)

                VStack(spacing: 4) {
                    battle(at: focus)
                    if game.battles.count > 1 { pageDots }
                }
                .position(x: w / 2, y: h * 0.42)
            }
            .frame(width: w, height: h)
            // Four corners pinned to the safe-area corners (the OS-standard inset
            // that follows the rounded display + top band), so deck/trump share a
            // left edge, discard/sword a right edge, and the top/bottom pairs a row.
            .overlay(alignment: .topLeading)     { countLabel(game.deckCount, "deck").padding(2) }
            .overlay(alignment: .topTrailing)    { countLabel(game.discardCount, "disc").padding(2) }
            .overlay(alignment: .bottomLeading)  { TokenCard(card: game.trump, size: 18).padding(.leading, 2) }
            .overlay(alignment: .bottomTrailing) { swordButton.padding(.trailing, 2) }
            .overlay(alignment: .bottom)         { handPeek }
        }
        .background(WColor.bg.ignoresSafeArea())
        .focusable(game.battles.count > 1)
        // Only page battles with the Crown when there's more than one — a
        // `from: 0, through: 0` range is degenerate and can crash the rotation.
        .modifier(BattlePager(crown: $crown, count: game.battles.count))
        .onAppear { game.load(gameId); crown = 0 }
    }

    // MARK: ring geometry

    private func angle(_ i: Int, n: Int) -> Double { .pi / 2 + Double(i) * 2 * .pi / Double(n) }
    private func pos(_ i: Int, n: Int, center: CGPoint, r: CGFloat) -> CGPoint {
        let a = angle(i, n: n)
        return CGPoint(x: center.x + r * CGFloat(cos(a)), y: center.y + r * CGFloat(sin(a)))
    }

    private func seat(_ s: SeatVM) -> some View {
        ZStack {
            if s.isDefender {
                ShieldShape().stroke(WColor.brass, lineWidth: 1.5)
                    .frame(width: 16, height: 19).offset(y: 1)   // tight around the count, clear of the name
            }
            Text("\(s.count)")
                .font(WFont.token(14))
                .foregroundStyle(s.isOut ? WColor.faint : (s.isSelf ? WColor.brass : WColor.ink))
            if s.isAttacker { Circle().fill(WColor.ink).frame(width: 3.5, height: 3.5).offset(y: 14) }
        }
        .frame(width: 22, height: 22)
    }

    private func countLabel(_ n: Int, _ label: String) -> some View {
        VStack(spacing: -1) {
            Text("\(n)").font(WFont.token(15)).foregroundStyle(WColor.ink)
            Text(label).font(WFont.label(7.5)).foregroundStyle(WColor.dim)
        }
    }

    // MARK: hero battle (one at a time) — small + tight

    private func battle(at i: Int) -> some View {
        Group {
            if game.battles.indices.contains(i) {
                let b = game.battles[i]
                HStack(spacing: -3) {
                    TokenCard(card: b.attack, size: 28)
                    if let cov = b.cover {
                        Text("›").font(WFont.token(14)).foregroundStyle(WColor.dim)
                        TokenCard(card: cov, size: 28)
                    }
                }
            } else {
                Text("—").font(WFont.token(18)).foregroundStyle(WColor.faint)
            }
        }
    }

    private var pageDots: some View {
        HStack(spacing: 4) {
            ForEach(0..<game.battles.count, id: \.self) { i in
                Circle().fill(i == focus ? WColor.brass : WColor.faint).frame(width: 3.5, height: 3.5)
            }
        }
    }

    // MARK: bottom pieces

    private var handPeek: some View {
        HStack(spacing: 1) {
            ForEach(Array(game.hand.prefix(5))) { c in TokenCard(card: c, size: 11) }
        }
    }

    private var swordButton: some View {
        SwordIcon(size: 15, color: WColor.bg)
            .frame(width: 28, height: 28)
            .background(Circle().fill(game.yourTurn ? WColor.brass : WColor.ink))
            .onTapGesture(perform: onPlay)
    }
}

/// Crown-paging over battles, applied only when there's more than one — avoids a
/// degenerate `from: 0, through: 0` rotation range.
private struct BattlePager: ViewModifier {
    @Binding var crown: Double
    let count: Int
    func body(content: Content) -> some View {
        if count > 1 {
            content.digitalCrownRotation($crown, from: 0, through: Double(count - 1),
                                         by: 1, sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
        } else {
            content
        }
    }
}
