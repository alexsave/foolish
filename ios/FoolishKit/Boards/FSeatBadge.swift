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
    // centred).
    private let cardW: CGFloat = 21
    private let cardH: CGFloat = 30
    /// The widest the fan may get, so a big hand cannot reach into the
    /// neighbouring seat on the ring.
    private let maxFanWidth: CGFloat = 62
    /// Spread per card at a comfortable count — narrowed below once the hand
    /// outgrows `maxFanWidth`.
    private let baseSpread: CGFloat = 6

    /// EVERY card in the hand gets a back. It used to be `min(handCount, 7)`,
    /// which meant a badge could read "11" over six visible cards - "I can see
    /// the number 11 but clearly see there are 6 cards in the hand". The count
    /// and the picture have to agree, so the cap moved off the number of cards
    /// and onto the WIDTH: the fan never grows past `maxFanWidth`, it just
    /// packs tighter, exactly as a real hand of cards does.
    private var visibleBacks: Int { max(handCount, 0) }

    /// Per-card offset that fits `visibleBacks` inside `maxFanWidth`.
    private var spread: CGFloat {
        let n = visibleBacks
        guard n > 1 else { return baseSpread }
        return min(baseSpread, (maxFanWidth - cardW) / CGFloat(n - 1))
    }

    public var body: some View {
        VStack(spacing: FSpace.xs) {
            // Round-5 M10: the known fix ("apply it") is full-opacity text plus
            // a REAL shadow, not a lighter foreground colour. .semibold plus a
            // slightly stronger shadow (0.7→0.85 opacity, 1.5→2 radius, a full
            // 1pt drop instead of 0.5) — still 12pt, still skipping the shadow
            // entirely for the onLight bubble variant, whose dark text on the
            // light bubble background never needed one.
            Text(name)
                .font(FType.body(12))
                .fontWeight(.semibold)
                .foregroundColor(isOut ? (onLight ? .black.opacity(0.4) : FColor.textDim)
                                       : (onLight ? .black.opacity(0.85) : FColor.textPrimary))
                .shadow(color: onLight ? .clear : .black.opacity(0.85), radius: onLight ? 0 : 2, y: onLight ? 0 : 1)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: 96)
                .minimumScaleFactor(0.7)

            ZStack {
                miniFan
                if handCount > 0 {
                    // Round-5 m9: a bare white-on-red numeral over the mini
                    // fan's own card backs reads as an iOS unread badge; the
                    // chip backs it with a near-black fill + the card backs'
                    // subdued edge red instead (see FCountChip).
                    FCountChip("\(handCount)", font: .system(size: 15, weight: .bold))
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
        // Round-5 m4: sizes raised to match the twin redesign in
        // MessageTableView (darker colors, pointed shield corners) — the icons
        // were unreadable at their old size on the weave, per the finding's
        // "no legibility ... on this background". FCheck 17→20, FShield 22→26.
        // The sword then went 23→32 on the owner's device review ("make the
        // sword icon larger"): it rotates 45° inside its box, so it needs a
        // bigger box than the shield to read the same size. Kept in step with
        // MessageTableView's selfRoleIndicator — one seat's role must not look
        // bigger than another's just because it is mine.
        HStack(spacing: FSpace.xs) {
            if thinking { ThinkingDots() }
            if saidGood {
                // Hand-built (not SF Symbols — unreliable under ImageRenderer bubble
                // snapshots, same reason FShield/FSword are hand-built).
                FCheck(size: 20)
            }
            if isDefender {
                FShield(size: 26)   // hand-built light-gray shield (larger)
            } else if isAttacker {
                FSword(size: 32)    // hand-built sword (rotated — needs the bigger box)
            }
        }
        // Tall enough for the largest glyph in the row (the 32pt sword), or it
        // clips the blade's corners.
        .frame(height: 32)
    }

    private var a11y: String {
        // Round-5 m2: these were hard-coded English literals while every
        // visible string in the app goes through FStrings — a ru/ko VoiceOver
        // user got an English board even though the screen itself was
        // localized.
        var parts = ["\(name), \(FStrings.t("ios.a11y.cards", ["n": "\(handCount)"]))"]
        if isDefender { parts.append(FStrings.t("ios.a11y.defending")) }
        else if isAttacker { parts.append(FStrings.t("ios.a11y.attacking")) }
        if saidGood { parts.append(FStrings.t("ios.a11y.saidgood")) }
        if thinking { parts.append(FStrings.t("ios.a11y.thinking")) }
        if isOut { parts.append(FStrings.t("ios.a11y.out")) }
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
