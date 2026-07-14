// TableScreen.swift — the glance screen (docs/WATCHOS_APP_PLAN.md §5.1). One
// screen, read-mostly: deck/discard corner micro-gauges, an opponent ring with
// the defender highlighted, the hero battle strip (`attack › cover`), and the
// bottom strip (trump · hand peek · Pl). Back-to-Games is the system nav bar,
// so all four corners stay free for state (§5.3).

import SwiftUI

struct TableScreen: View {
    @ObservedObject var game: MockGame
    let onPlay: () -> Void

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                if game.yourTurn { turnVignette(h: h) }               // §3.1 cue 4: brass on your move

                // Corners as micro-gauges (§3.1 cue 2)
                RingGauge(count: game.deckCount, fraction: CGFloat(game.deckCount) / CGFloat(game.deckMax),
                          fill: false, tint: WColor.dim)
                    .position(x: 17, y: 12)
                RingGauge(count: game.discardCount, fraction: CGFloat(game.discardCount) / CGFloat(game.deckMax),
                          fill: true, tint: WColor.dim)
                    .position(x: w - 17, y: 12)

                // Opponent ring — bare hand-count numerals; defender gets the brass ring.
                ForEach(game.opponents) { opp in
                    seat(opp)
                        .position(ringPosition(opp.id, count: game.opponents.count, in: geo.size))
                }

                // Hero: the battle strip, largest tokens on screen.
                battleStrip
                    .position(x: w / 2, y: h * 0.47)

                bottomStrip
                    .frame(width: w - 6)
                    .position(x: w / 2, y: h - 13)
            }
            .frame(width: w, height: h)
        }
        .background(WColor.bg)
        .ignoresSafeArea(edges: .bottom)
    }

    // MARK: ring

    /// θᵢ = −90° + i·360°/n, R ≈ 0.42·min(w,h); you are implicit at 6 o'clock
    /// (not drawn — the bottom strip is you). §5.1 seat math.
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
                .font(WFont.token(20))
                .foregroundStyle(opp.isOut ? WColor.faint : WColor.ink)
                .frame(width: 30, height: 30)
                .overlay(Circle().strokeBorder(isDefender ? WColor.brass : .clear, lineWidth: 2))
            if isAttacker {
                Circle().fill(WColor.ink).frame(width: 4, height: 4)   // attacker dot
            }
        }
    }

    // MARK: battle strip (hero)

    private var battleStrip: some View {
        HStack(spacing: 10) {
            if game.battles.isEmpty {
                Text("—").font(WFont.token(20)).foregroundStyle(WColor.faint)
            } else {
                ForEach(game.battles) { b in
                    HStack(spacing: 3) {
                        TokenCard(card: b.attack, size: 34, trump: b.attack.suit == game.trumpSuit)
                        if let cov = b.cover {
                            Text("›").font(WFont.token(24)).foregroundStyle(WColor.dim)
                            TokenCard(card: cov, size: 34, trump: cov.suit == game.trumpSuit)
                        }
                    }
                }
            }
        }
    }

    // MARK: bottom strip — trump · hand peek · Pl

    private var bottomStrip: some View {
        HStack(spacing: 5) {
            HStack(spacing: 2) {
                Text("Tr").font(WFont.label(11)).foregroundStyle(WColor.dim)
                Text(game.trump.label).font(WFont.token(15)).foregroundStyle(game.trumpSuit.color)
                Text(game.trumpSuit.glyph).font(WFont.token(9)).foregroundStyle(game.trumpSuit.color)
            }
            Spacer(minLength: 2)
            // Hand peek — first five values, suit-colored, read-only (§4).
            HStack(spacing: 4) {
                ForEach(Array(game.hand.prefix(5))) { c in
                    Text(c.label).font(WFont.token(14)).foregroundStyle(c.suit.color)
                }
            }
            Spacer(minLength: 2)
            Text("Pl")
                .font(WFont.label(13))
                .foregroundStyle(WColor.bg)
                .padding(.horizontal, 8).padding(.vertical, 3)
                .background(Capsule().fill(game.yourTurn ? WColor.brass : WColor.ink))
        }
        .padding(.horizontal, 4)
        .contentShape(Rectangle())
        .onTapGesture(perform: onPlay)     // whole strip → Action (§5.1)
    }

    private func turnVignette(h: CGFloat) -> some View {
        LinearGradient(colors: [.clear, WColor.brass.opacity(0.16)], startPoint: .top, endPoint: .bottom)
            .ignoresSafeArea()
    }
}

/// A corner micro-gauge (§3.1 cue 2): a numeral inside a ring that depletes
/// (deck) or fills (discard). The ring shape IS the "how long is this game"
/// signal at a glance.
struct RingGauge: View {
    let count: Int
    let fraction: CGFloat
    let fill: Bool         // false = depleting (deck), true = filling (discard)
    let tint: Color

    var body: some View {
        let f = max(0, min(1, fraction))
        ZStack {
            Circle().stroke(WColor.faint, lineWidth: 2)
            Circle()
                .trim(from: 0, to: fill ? f : 1)
                .stroke(tint, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .opacity(fill ? 1 : f)          // deck: fade the ring as it drains
            Text("\(count)").font(WFont.token(13)).foregroundStyle(WColor.ink)
        }
        .frame(width: 28, height: 28)
    }
}
