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
    /// Read once here rather than at each colour, for the same reason FCard
    /// does: the ink and the shadow behind it have to agree about which weave
    /// this badge is sitting on.
    @Environment(\.colorScheme) private var scheme
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
    /// Which seat this badge is, when the board wants its role mark to be a
    /// take-off or landing pad for a flight (round 16). nil on the boards that
    /// only ever draw a seat - the bubble snapshot, the gallery, the rules -
    /// where there is no `boardSpace` to publish into and nothing flies.
    public let seat: Int?
    /// This seat's mark is in the air right now as a flight ghost, so the badge
    /// must not draw its own copy (FRoleMotion).
    public let markFlying: Bool

    public init(name: String, handCount: Int, isDefender: Bool = false,
                isAttacker: Bool = false, saidGood: Bool = false,
                thinking: Bool = false, isOut: Bool = false, onLight: Bool = false,
                seat: Int? = nil, markFlying: Bool = false) {
        self.name = name
        self.handCount = handCount
        self.isDefender = isDefender
        self.isAttacker = isAttacker
        self.saidGood = saidGood
        self.thinking = thinking
        self.isOut = isOut
        self.onLight = onLight
        self.seat = seat
        self.markFlying = markFlying
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
                .foregroundColor(nameInk)
                .shadow(color: nameShadow, radius: nameShadow == .clear ? 0 : 2,
                        y: nameShadow == .clear ? 0 : 1)
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
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y)
    }

    /// Is this badge sitting on a LIGHT ground? The beige message bubble always
    /// is; the wool board is in light mode and is not in dark (the weave is a
    /// beige/red plaid one way and a dark grey one the other - see WoolTexture).
    /// Both the ink and the shadow hang off this one question.
    static func onLightGround(onLight: Bool, scheme: ColorScheme) -> Bool {
        onLight || scheme != .dark
    }

    /// ROUND 16, the owner: "players text that are out are invisible against the
    /// wool background light mode. Make them just dark gray instead of
    /// decreasing opacity."
    ///
    /// They were invisible twice over: a dim SAGE (`textDim`, picked for a dark
    /// board) and then the whole badge at 0.45 opacity on top of it, which on
    /// the light-mode weave left roughly nothing. Being out is now said with
    /// INK, not with transparency - the badge draws at full strength and an out
    /// player's name simply goes dark grey on a light ground. The container
    /// `.opacity` is gone entirely rather than reduced: dimming a whole view is
    /// what made a legible colour unreadable, and it would do the same to any
    /// colour picked to replace it.
    ///
    /// Dark mode is unchanged (`textDim` on the dark weave already reads, and
    /// the report is light-mode); a dark grey there would be the same mistake
    /// pointing the other way.
    static func nameInk(isOut: Bool, onLight: Bool, scheme: ColorScheme) -> Color {
        if isOut {
            return onLightGround(onLight: onLight, scheme: scheme)
                 ? FColor.textOut : FColor.textDim
        }
        return onLight ? .black.opacity(0.85) : FColor.textPrimary
    }
    private var nameInk: Color { Self.nameInk(isOut: isOut, onLight: onLight, scheme: scheme) }

    /// The bone names are carried by a hard black shadow, which is what lets
    /// light-on-light work on the pale weave. Dark ink on a light ground needs
    /// no such help and is muddied by it, so an out name drops it.
    static func nameShadow(isOut: Bool, onLight: Bool, scheme: ColorScheme) -> Color {
        let onLightGround = onLightGround(onLight: onLight, scheme: scheme)
        return (onLight || (isOut && onLightGround)) ? .clear : .black.opacity(0.85)
    }
    private var nameShadow: Color { Self.nameShadow(isOut: isOut, onLight: onLight, scheme: scheme) }

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

    /// The ONE mark this seat wears. Never two: the kernel rejects a defender's
    /// `good` (game.c handle_good), and `showsSword` already stands the sword
    /// down for a seat that has said it - so shield, sword and check are
    /// mutually exclusive in every state the engine can produce, which is what
    /// lets them be one coin with three faces (FRoleMotion).
    var mark: RoleMarkKind? {
        if saidGood { return .check }
        if isDefender { return .shield }
        if isAttacker { return .sword }
        return nil
    }

    private var roleRow: some View {
        // Sizes come from `FRoleMark` - the ONE table both role rows read, so a
        // seat's mark cannot end up a different size from mine. The marks are
        // hand-built (not SF Symbols, which are unreliable under ImageRenderer
        // bubble snapshots) and painted in the shared `FRoleInk`.
        HStack(spacing: FSpace.xs) {
            if thinking { ThinkingDots() }
            FRoleCoin(kind: mark, flying: markFlying)
                .background(GeometryReader { g in
                    Color.clear.preference(
                        key: RoleMarkFramesKey.self,
                        value: seat.map { [$0: g.frame(in: .named(boardSpace))] } ?? [:])
                })
        }
        // Tall enough for the largest glyph in the row, or it clips the blade.
        .frame(height: FRoleMark.rowHeight)
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
