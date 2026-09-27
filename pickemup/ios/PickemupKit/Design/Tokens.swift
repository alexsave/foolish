// COPIED from foolish/ios/FoolishKit/DesignSystem/Tokens.swift at c3d99192 - replaced by lift step S2
//
// Trimmed to what this product draws with, and one addition: the felt text
// treatment (`onFeltText`), because this table is always foolish's felt
// (UI_DECISIONS U1), a DARK surface in both schemes, where foolish's
// `onTableText` was written for the light wool. The values are foolish's.

import CPickemup
import SwiftUI

public enum FColor {
    public static let card        = Color(hex: 0xF4EFE6)   // bone white, the face (U1)
    public static let ink         = Color(hex: 0x17140F)
    public static let textPrimary = Color(hex: 0xEDE9DF)   // --ink in UI.html
    public static let textDim     = Color(hex: 0x9AA69E)   // --inkdim
    public static let win         = Color(hex: 0xD8B24A)   // brass: the turn bar, drop bands, OUT (U5)
    public static let amber       = Color(hex: 0xD9A13A)   // the catch ring (U13)
    public static let red         = Color(hex: 0xD1584A)   // Caught you!, the skip slash
    public static let selRed      = Color(hex: 0xFF2A22)   // foolish's selection ring
    public static let deepRed     = Color(hex: 0x8B1A1A)   // the card edge, locked (round 44)
}

/// 4pt grid (foolish's section 5.2).
public enum FSpace {
    public static let xs: CGFloat = 4
    public static let s: CGFloat = 8
    public static let m: CGFloat = 12
    public static let l: CGFloat = 16
    public static let xl: CGFloat = 24
}

public enum FType {
    public static func body(_ size: CGFloat = 15) -> Font { .system(size: size, weight: .regular) }
    public static func title(_ size: CGFloat = 22) -> Font { .system(size: size, weight: .semibold) }
}

/// foolish's two motion tokens, their numbers the kernel's (pk_beats.h), so
/// no duration is typed in Swift.
public enum FMotion {
    /// The ONE spring for all card movement (card-spring).
    public static let card: Animation = .spring(response: Double(PK_T_SPRING) / 1000,
                                                dampingFraction: Double(PK_T_SPRING_DAMP) / 100)
    /// Chrome: ease-out.
    public static let chrome: Animation = .easeOut(duration: Double(PK_T_CHROME) / 1000)
    /// A pill or a tile pressed.
    public static let press: Animation = .easeOut(duration: Double(PK_T_PRESS) / 1000)
}

extension Color {
    /// 0xRRGGBB literal -> Color.
    init(hex: UInt32, alpha: Double = 1) {
        let r = Double((hex >> 16) & 0xFF) / 255
        let g = Double((hex >> 8) & 0xFF) / 255
        let b = Double(hex & 0xFF) / 255
        self.init(.sRGB, red: r, green: g, blue: b, opacity: alpha)
    }
}

public extension View {
    /// Text on `WoodFill`: heavy white with a dark drop shadow; `dimmed` is a
    /// lighter ink on a fully opaque plank, never a see-through control.
    func onWoodText(dimmed: Bool = false) -> some View {
        self.fontWeight(.heavy)
            .foregroundStyle(dimmed ? Color.white.opacity(0.55) : .white)
            .shadow(color: .black.opacity(dimmed ? 0.3 : 0.5), radius: 1, y: 1)
    }

    /// Text straight on the felt: bone ink over a dark shadow (UI.html
    /// `.status .sh`, `text-shadow: 0 1px 2px rgba(0,0,0,.6)`).
    func onFeltText(_ ink: Color = FColor.textPrimary) -> some View {
        self.foregroundStyle(ink)
            .shadow(color: .black.opacity(0.6), radius: 2, y: 1)
    }
}
