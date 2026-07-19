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
    public static func suitColor(_ suit: Suit) -> Color { suit.isRed ? accent : ink }
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
