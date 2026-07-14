// TableScreen.swift — the glance screen (docs/WATCHOS_APP_PLAN.md §5.1). One
// screen: deck/discard counts (top corners), the FULL seat ring incl. you at
// 6 o'clock (names arced along the ring), the hero battle (ONE attack/cover pair,
// the Crown pages through them), and the bottom strip (flipped trump · hand peek
// · a round sword → Action). Black canvas.

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
            let center = CGPoint(x: w / 2, y: h * 0.45)
            let r = 0.34 * min(w, h)
            ZStack {
                // seats
                ForEach(Array(seats.enumerated()), id: \.element.id) { i, s in
                    seat(s).position(pos(i, n: seats.count, center: center, r: r))
                }
                // names arced along the whole ring, just outside the seats
                RingNames(items: seats.enumerated().map { i, s in
                    RingNames.Item(id: s.id, angle: angle(i, n: seats.count), name: s.name,
                                   color: s.isSelf ? WColor.brass.opacity(0.9) : WColor.dim)
                }, radius: r + 13, fontSize: 8.5)
                .position(center)

                VStack(spacing: 5) {
                    battle(at: focus)
                    if game.battles.count > 1 { pageDots }
                }
                .position(x: w / 2, y: h * 0.44)

                // deck / discard — top corners, drawn on top of the ring
                countLabel(game.deckCount, "deck").position(x: 22, y: 24)
                countLabel(game.discardCount, "disc").position(x: w - 24, y: 24)

                bottomStrip.frame(width: w - 4).position(x: w / 2, y: h - 12)
            }
            .frame(width: w, height: h)
        }
        .background(WColor.bg)
        .ignoresSafeArea(edges: .bottom)
        .focusable()
        .digitalCrownRotation($crown, from: 0, through: Double(max(game.battles.count - 1, 0)),
                              by: 1, sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
        .onAppear { game.load(gameId); crown = 0 }
    }

    // MARK: ring geometry

    private func angle(_ i: Int, n: Int) -> Double { .pi / 2 + Double(i) * 2 * .pi / Double(n) }
    private func pos(_ i: Int, n: Int, center: CGPoint, r: CGFloat) -> CGPoint {
        let a = angle(i, n: n)
        return CGPoint(x: center.x + r * CGFloat(cos(a)), y: center.y + r * CGFloat(sin(a)))
    }

    // MARK: seat (small circle, tight shield for the defender)

    private func seat(_ s: SeatVM) -> some View {
        ZStack {
            Circle().fill(Color(white: 0.14)).frame(width: 24, height: 24)
            if s.isDefender {
                ShieldShape().stroke(WColor.brass, lineWidth: 1.8)
                    .frame(width: 22, height: 27).offset(y: -1)      // tight around the count
            }
            Text("\(s.count)")
                .font(WFont.token(14))
                .foregroundStyle(s.isOut ? WColor.faint : (s.isSelf ? WColor.brass : WColor.ink))
            if s.isAttacker {
                Circle().fill(WColor.ink).frame(width: 4, height: 4).offset(y: 16)
            }
        }
    }

    private func countLabel(_ n: Int, _ label: String) -> some View {
        VStack(spacing: -1) {
            Text("\(n)").font(WFont.token(15)).foregroundStyle(WColor.ink)
            Text(label).font(WFont.label(7.5)).foregroundStyle(WColor.dim)
        }
    }

    // MARK: hero battle (one at a time)

    private func battle(at i: Int) -> some View {
        Group {
            if game.battles.indices.contains(i) {
                let b = game.battles[i]
                HStack(spacing: 4) {
                    TokenCard(card: b.attack, size: 38)
                    if let cov = b.cover {
                        Text("›").font(WFont.token(24)).foregroundStyle(WColor.dim)
                        TokenCard(card: cov, size: 38)
                    }
                }
            } else {
                Text("—").font(WFont.token(20)).foregroundStyle(WColor.faint)
            }
        }
    }

    private var pageDots: some View {
        HStack(spacing: 4) {
            ForEach(0..<game.battles.count, id: \.self) { i in
                Circle().fill(i == focus ? WColor.brass : WColor.faint).frame(width: 4, height: 4)
            }
        }
    }

    // MARK: bottom strip — flipped trump · hand peek · round sword

    private var bottomStrip: some View {
        HStack(spacing: 4) {
            TokenCard(card: game.trump, size: 14)
            Spacer(minLength: 2)
            HStack(spacing: 2) {
                ForEach(Array(game.hand.prefix(5))) { c in TokenCard(card: c, size: 12) }
            }
            Spacer(minLength: 2)
            SwordIcon(size: 16, color: WColor.bg)
                .frame(width: 32, height: 32)
                .background(Circle().fill(game.yourTurn ? WColor.brass : WColor.ink))
        }
        .padding(.horizontal, 3)
        .contentShape(Rectangle())
        .onTapGesture(perform: onPlay)
    }
}
