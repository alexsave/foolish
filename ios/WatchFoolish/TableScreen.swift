// TableScreen.swift — the glance screen (docs/WATCHOS_APP_PLAN.md §5.1). One
// screen, read-mostly: deck/discard corner micro-gauges, an opponent ring with
// the defender highlighted, and the hero battle — ONE attack/cover pair at a
// time, the Crown pages through them. Bottom strip: flipped trump · hand peek ·
// the sword (→ Action). Back-to-Games is the system nav bar (§5.3).

import SwiftUI

struct TableScreen: View {
    @ObservedObject var game: MockGame
    var gameId: String = "g1"
    let onPlay: () -> Void

    @State private var crown: Double = 0

    private var focus: Int { min(max(Int(crown.rounded()), 0), max(game.battles.count - 1, 0)) }

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                if game.yourTurn { turnVignette }                    // §3.1 cue 4: brass on your move

                RingGauge(count: game.deckCount, fraction: CGFloat(game.deckCount) / CGFloat(game.deckMax),
                          fill: false, tint: WColor.dim)
                    .position(x: 17, y: 12)
                RingGauge(count: game.discardCount, fraction: CGFloat(game.discardCount) / CGFloat(game.deckMax),
                          fill: true, tint: WColor.dim)
                    .position(x: w - 17, y: 12)

                ForEach(game.opponents) { opp in
                    seat(opp).position(ringPosition(opp.id, count: game.opponents.count, in: geo.size))
                }

                // Hero: the focused battle only, largest tokens on screen.
                VStack(spacing: 5) {
                    battle(at: focus)
                    if game.battles.count > 1 { pageDots }
                }
                .position(x: w / 2, y: h * 0.46)

                bottomStrip.frame(width: w - 4).position(x: w / 2, y: h - 13)
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

    // MARK: opponent ring

    /// θᵢ = −90° + i·360°/n, R ≈ 0.42·min(w,h); you are implicit at 6 o'clock.
    private func ringPosition(_ i: Int, count: Int, in size: CGSize) -> CGPoint {
        let cx = size.width / 2, cy = size.height * 0.40
        let r = 0.42 * min(size.width, size.height)
        let theta = -Double.pi / 2 + Double(i) * 2 * .pi / Double(max(count, 1))
        return CGPoint(x: cx + r * CGFloat(cos(theta)), y: cy + r * CGFloat(sin(theta)))
    }

    private func seat(_ opp: Opponent) -> some View {
        let isDefender = opp.id == game.defenderSeat
        let isAttacker = opp.id == game.attackerSeat
        return VStack(spacing: 1) {
            Text("\(opp.handCount)")
                .font(WFont.token(18))
                .foregroundStyle(opp.isOut ? WColor.faint : WColor.ink)
                .frame(width: 28, height: 28)
                .overlay(Circle().strokeBorder(isDefender ? WColor.brass : .clear, lineWidth: 2))
            if isAttacker {
                Circle().fill(WColor.ink).frame(width: 4, height: 4)
            }
        }
    }

    // MARK: hero battle (one at a time)

    private func battle(at i: Int) -> some View {
        Group {
            if game.battles.indices.contains(i) {
                let b = game.battles[i]
                HStack(spacing: 4) {
                    TokenCard(card: b.attack, size: 40)
                    if let cov = b.cover {
                        Text("›").font(WFont.token(26)).foregroundStyle(WColor.dim)
                        TokenCard(card: cov, size: 40)
                    }
                }
            } else {
                Text("—").font(WFont.token(22)).foregroundStyle(WColor.faint)
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

    // MARK: bottom strip — flipped trump · hand peek · sword

    private var bottomStrip: some View {
        HStack(spacing: 4) {
            TokenCard(card: game.trump, size: 15)          // the flipped trump (no "Tr" label)
            Spacer(minLength: 2)
            HStack(spacing: 3) {
                ForEach(Array(game.hand.prefix(5))) { c in
                    TokenCard(card: c, size: 13)
                }
            }
            Spacer(minLength: 2)
            SwordIcon(size: 15, color: WColor.bg)          // Pl → the sword (→ Action)
                .padding(.horizontal, 9).padding(.vertical, 5)
                .background(Capsule().fill(game.yourTurn ? WColor.brass : WColor.ink))
        }
        .padding(.horizontal, 3)
        .contentShape(Rectangle())
        .onTapGesture(perform: onPlay)
    }

    private var turnVignette: some View {
        LinearGradient(colors: [.clear, WColor.brass.opacity(0.16)], startPoint: .top, endPoint: .bottom)
            .ignoresSafeArea()
    }
}

/// A corner micro-gauge (§3.1 cue 2): a numeral inside a ring that depletes
/// (deck) or fills (discard).
struct RingGauge: View {
    let count: Int
    let fraction: CGFloat
    let fill: Bool
    let tint: Color

    var body: some View {
        let f = max(0, min(1, fraction))
        ZStack {
            Circle().stroke(WColor.faint, lineWidth: 2)
            Circle()
                .trim(from: 0, to: fill ? f : 1)
                .stroke(tint, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .opacity(fill ? 1 : f)
            Text("\(count)").font(WFont.token(13)).foregroundStyle(WColor.ink)
        }
        .frame(width: 28, height: 28)
    }
}
