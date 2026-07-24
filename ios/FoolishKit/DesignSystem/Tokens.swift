// Tokens.swift — the single source of the "Gosizdat Card Table" identity
// (§5.1, §5.2). One theme, dark-first: deep matte felt, bone-white cards, a
// single Soviet-red accent, brass for wins. No gradients (except a subtle table
// vignette), no glassmorphism, at most ONE accent color on screen at a time.
//
// Everything visual references these tokens — never a raw hex or magic number
// in a view. This file IS the style mandate; the DEBUG GalleryView renders it.

import SwiftUI

public enum FColor {
    // Surfaces. "Light" mode only lifts surfaces ~6% — the app is dark-first
    // and forces UIUserInterfaceStyle=Dark, so the light values exist mainly
    // for snapshot coverage and future opt-in.
    public static let table   = Color(hex: 0x14231C)   // deep green-black felt
    public static let tableLo = Color(hex: 0x1B2E24)   // light-mode felt (+lift)
    /// The web's fallback beige (--color-concrete / --color-wood-base, #F5E6C8) —
    /// the solid colour to show BEHIND a texture before it renders (the wool base,
    /// the message bubble), so a bare moment reads beige, never green or red.
    public static let fallback = Color(hex: 0xF5E6C8)
    public static let surface = Color(hex: 0x1E2A24)
    public static let card    = Color(hex: 0xF4EFE6)   // bone white
    public static let ink     = Color(hex: 0x17140F)   // pips / type on a card
    public static let accent  = Color(hex: 0xC82B24)   // Soviet red
    public static let textPrimary = Color(hex: 0xEDE9DF)
    public static let textDim     = Color(hex: 0x9AA69E)
    public static let win     = Color(hex: 0xD8B24A)   // brass — victories, streaks

    /// Suit ink on a bone card: red suits use the accent red, black suits the ink.
    public static func suitColor(_ suit: Suit) -> Color { suitColor(suit, scheme: .light) }

    /// The same, for a glyph drawn straight on the BOARD rather than on a card
    /// (the deck well's bare trump mark is the only such glyph today).
    ///
    /// On a dark board the near-black `ink` a spade or a club would take is
    /// invisible against the walnut weave - and the trump suit is not
    /// decoration, it is the one piece of rules state that is only ever shown
    /// as a colour and a shape. So black suits go bone in dark mode, exactly
    /// like the card faces do; red suits keep the accent red, which carries on
    /// either weave.
    public static func suitColor(_ suit: Suit, scheme: ColorScheme) -> Color {
        if suit.isRed { return accent }
        return scheme == .dark ? textPrimary : ink
    }
}

public enum FRadius {
    public static let card: CGFloat = 10
    public static let sheet: CGFloat = 16
    public static let chip: CGFloat = 999
}

/// 4pt grid — the only spacing values allowed (§5.2).
public enum FSpace {
    public static let xs: CGFloat = 4
    public static let s: CGFloat = 8
    public static let m: CGFloat = 12
    public static let l: CGFloat = 16
    public static let xl: CGFloat = 24
    public static let xxl: CGFloat = 32
}

public enum FType {
    public static func body(_ size: CGFloat = 15) -> Font { .system(size: size, weight: .regular, design: .default) }
    public static func title(_ size: CGFloat = 22) -> Font { .system(size: size, weight: .semibold, design: .default) }
    /// The signature: SF Compressed Bold for BIG numerals (deck count, timers,
    /// ranks). Used NOWHERE else. `.width(.compressed)` is the SF Compressed axis.
    public static func numeral(_ size: CGFloat) -> Font {
        .system(size: size, weight: .bold).width(.compressed)
    }
}

public enum FMotion {
    /// The ONE spring for ALL card movement (§5.2).
    public static let card: Animation = .spring(response: 0.32, dampingFraction: 0.82)
    /// Chrome (buttons, sheets appearing): 150ms ease-out. Nothing over 400ms.
    public static let chrome: Animation = .easeOut(duration: 0.15)
    /// Deal stagger between cards.
    public static let dealStagger: Double = 0.04

    /// Reduce Motion swaps the spring for a cross-fade (§5.4, §16.E5).
    public static func cardMotion(reduceMotion: Bool) -> Animation {
        reduceMotion ? .easeInOut(duration: 0.18) : card
    }
}

extension Color {
    /// 0xRRGGBB literal → Color. Kept private-in-spirit to Tokens; views use the
    /// named FColor tokens, not this.
    init(hex: UInt32, alpha: Double = 1) {
        let r = Double((hex >> 16) & 0xFF) / 255
        let g = Double((hex >> 8) & 0xFF) / 255
        let b = Double(hex & 0xFF) / 255
        self.init(.sRGB, red: r, green: g, blue: b, opacity: alpha)
    }
}

// MARK: - Text on a surface (round-6 #17)
//
// Two textures, two treatments — the owner's exact words: "The thicker white
// text we use for the ranks is good. Use it everywhere we put text on wood.
// Thicker black text over wool though." The "ranks" are the game-over rank
// column (MessageTableView.FGameOverList): `.heavy` weight, flat white, a
// dark drop shadow. `onWoodText` lifts that treatment out to every wood
// surface; `onWoolText` is its wool mirror — round-5 M10 already worked out
// that dark ink on the lighter wool weave wants a LIGHT shadow, the inverse
// of wood's dark one, or the shadow adds nothing. Round-6 only asked for more
// WEIGHT on top of that pairing, so weight is the one thing both share.
//
// DARK MODE splits the pair. Wood was ALWAYS the dark surface, so its white
// ink is right in both schemes and `onWoodText` is unchanged (a darker plank
// only helps it). Wool was the LIGHT surface, and that is the one assumption
// dark mode breaks: black ink on a walnut weave is invisible, so the dark
// wool takes wood's treatment instead — bone ink over a dark shadow. Note
// what does NOT change: the weight, the shadow radius, and the dimmed ratio.
// Only which end of the scale the ink sits at flips, which is the whole of
// "a half-dark board is worse than none" expressed as code.
public extension View {
    /// Text sitting directly on `WoodFill` — wooden buttons, the game-over
    /// plank, any wood-surfaced control. `dimmed` is round-6 #19's disabled
    /// state: NOT reduced opacity on the whole control (that let the wool
    /// behind a disabled button show straight through it), just a lighter
    /// ink on an otherwise fully opaque surface.
    func onWoodText(dimmed: Bool = false) -> some View {
        self.fontWeight(.heavy)
            .foregroundStyle(dimmed ? Color.white.opacity(0.55) : .white)
            .shadow(color: .black.opacity(dimmed ? 0.3 : 0.5), radius: 1, y: 1)
    }

    /// Text sitting directly on the wool weave (`WoolBackground` / `WoolWeave`
    /// / the bubble snapshot's own wool crop) — labels, headlines, captions.
    /// `dimmed` mirrors `onWoodText`'s disabled case.
    func onWoolText(dimmed: Bool = false) -> some View {
        modifier(OnWoolText(dimmed: dimmed))
    }
}

/// A ViewModifier, not a plain `some View` chain, for one reason: it has to read
/// `@Environment(\.colorScheme)`, and a `View` extension method has no
/// environment of its own to read. Being a modifier also means every call site
/// picks up the scheme it is actually drawn in — including the bubble snapshot,
/// which pins `.light` around its whole ImageRenderer content and therefore gets
/// the light treatment even on a sender's dark phone.
private struct OnWoolText: ViewModifier {
    @Environment(\.colorScheme) private var scheme
    let dimmed: Bool

    func body(content: Content) -> some View {
        // Bone rather than pure white on the dark weave: it is the same
        // `textPrimary` every other light-on-dark label in the app uses
        // (FSeatBadge's names sit two inches away on the same board), and a
        // second, whiter white beside it would read as two different inks.
        let ink: Color = scheme == .dark ? FColor.textPrimary : FColor.ink
        let shadow: Color = scheme == .dark ? .black : .white
        return content
            .fontWeight(.heavy)
            .foregroundStyle(dimmed ? ink.opacity(0.55) : ink)
            .shadow(color: shadow.opacity(dimmed ? 0.3 : 0.5), radius: 2)
    }
}
