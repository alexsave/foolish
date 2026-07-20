// FSeatBadge.swift — an opponent seat, matching the web's PlayerRing seat
// (src/components/GameDisplay/PlayerRing.tsx `CardsVisual`): the name on top, a
// mini fan of red card backs with the hand count centred on it, and the role
// marks (defender shield / attacker flame / "good" / thinking). No avatars.
//
// Per the 2026-07-16 owner decision (IOS_APP_DESIGN §17.10) the app copies the
// web layout rather than the earlier count-chip design.

import SwiftUI
import Foundation   // sin(_:) for the thinking-dots pulse

public struct FSeatBadge: View {
    public let name: String
    public let handCount: Int
    public let isDefender: Bool
    public let isAttacker: Bool
    public let saidGood: Bool
    public let thinking: Bool
    public let isOut: Bool
    /// Dark name text (no shadow) for a LIGHT background — the beige message
    /// bubble. The wool board keeps bone text + a shadow (onLight = false).
    public let onLight: Bool

    public init(name: String, handCount: Int, isDefender: Bool = false,
                isAttacker: Bool = false, saidGood: Bool = false,
                thinking: Bool = false, isOut: Bool = false, onLight: Bool = false) {
        self.name = name
        self.handCount = handCount
        self.isDefender = isDefender
        self.isAttacker = isAttacker
        self.saidGood = saidGood
        self.thinking = thinking
        self.isOut = isOut
        self.onLight = onLight
    }

    // Mini back geometry (web CardsVisual: 25pt wide, spread 10pt/card, count
    // centred). Capped so a big hand doesn't fan into the neighbouring seat.
    private let cardW: CGFloat = 24
    private let cardH: CGFloat = 34
    private let spread: CGFloat = 7
    private var visibleBacks: Int { min(max(handCount, 0), 7) }

    public var body: some View {
        VStack(spacing: FSpace.xs) {
            Text(name)
                .font(FType.body(12))
                .foregroundColor(isOut ? (onLight ? .black.opacity(0.4) : FColor.textDim)
                                       : (onLight ? .black.opacity(0.85) : FColor.textPrimary))
                .shadow(color: onLight ? .clear : .black.opacity(0.7), radius: onLight ? 0 : 1.5, y: onLight ? 0 : 0.5)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: 96)
                .minimumScaleFactor(0.7)

            ZStack {
                miniFan
                if handCount > 0 {
                    Text("\(handCount)")
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(.white)
                        .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
                }
            }
            .frame(width: cardW + spread * CGFloat(max(visibleBacks - 1, 0)) + 6, height: cardH + 4)
            // (no role ring around the mini-fan - the role is shown by the row of
            // icons below; the gold/red border read as clutter)

            roleRow
        }
        .opacity(isOut ? 0.45 : 1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y)
    }

    /// The overlapping mini card backs, centred (web spreads by index - mid).
    private var miniFan: some View {
        let n = visibleBacks
        let mid = Double(max(n - 1, 0)) / 2
        return ZStack {
            if n == 0 {
                // An out / empty seat shows nothing but its name (web parity).
                Color.clear.frame(width: cardW, height: cardH)
            } else {
                ForEach(0..<n, id: \.self) { i in
                    FCard(card: nil, backSeed: UInt64(7 + i), size: CGSize(width: cardW, height: cardH))
                        .offset(x: CGFloat(Double(i) - mid) * spread)
                }
            }
        }
    }

    private var roleRow: some View {
        HStack(spacing: FSpace.xs) {
            if thinking { ThinkingDots() }
            if saidGood {
                // Hand-built (not SF Symbols — unreliable under ImageRenderer bubble
                // snapshots, same reason FShield/FSword are hand-built).
                FCheck(size: 14)
            }
            if isDefender {
                FShield(size: 19)   // hand-built light-gray shield (larger)
            } else if isAttacker {
                FSword(size: 16)    // hand-built sword
            }
        }
        .frame(height: 19)
    }

    private var a11y: String {
        var parts = ["\(name), \(handCount) cards"]
        if isDefender { parts.append("defending") }
        else if isAttacker { parts.append("attacking") }
        if saidGood { parts.append("said good") }
        if thinking { parts.append("thinking") }
        if isOut { parts.append("out") }
        return parts.joined(separator: ", ")
    }
}

/// A three-dot "thinking" pulse (Reduce-Motion falls back to a static dot row).
public struct ThinkingDots: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var phase = 0.0
    public init() {}
    public var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<3, id: \.self) { i in
                Circle().fill(FColor.textDim)
                    .frame(width: 4, height: 4)
                    .opacity(reduceMotion ? 0.6 : 0.3 + 0.7 * abs(sin(phase + Double(i) * 0.6)))
            }
        }
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.linear(duration: 0.9).repeatForever(autoreverses: false)) { phase = .pi * 2 }
        }
        .accessibilityHidden(true)
    }
}
