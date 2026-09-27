// COPIED from pickemup/ios/PickemupKit/Design/Tokens.swift at 8e216923 (itself COPIED from foolish/ios/FoolishKit/DesignSystem/Tokens.swift at c3d99192) - replaced by the shared-design lift
//
// Trimmed to what this product draws with. The values are foolish's, and the
// felt text treatment (`onFeltText`) is Pick 'Em Up's: this table is always
// foolish's felt, a DARK surface in both schemes.
//
// Two additions for dice: the die's own face and pip ink (T50), which are
// foolish's card bone and card ink, so a die reads as the same material as a
// card.

import SwiftUI

public enum FColor {
    public static let card        = Color(hex: 0xF4EFE6)   // bone white, a card face and a die face
    public static let ink         = Color(hex: 0x17140F)   // pips
    public static let textPrimary = Color(hex: 0xEDE9DF)
    public static let textDim     = Color(hex: 0x9AA69E)
    public static let win         = Color(hex: 0xD8B24A)   // brass: the turn bar, the kept ring
    public static let deepRed     = Color(hex: 0x8B1A1A)   // foolish's card edge, locked (round 44)
}

/// 4pt grid (foolish's section 5.2).
public enum FSpace {
    public static let xs: CGFloat = 4
    public static let s: CGFloat = 8
    public static let m: CGFloat = 12
    public static let l: CGFloat = 16
    public static let xl: CGFloat = 24
}

/// foolish's motion tokens.
/// Chrome motion only: every game motion is tb_beats.h's (BeatPlayer).
/// pickemup reads these from pk_beats.h (PK_T_SPRING and friends); tb_beats.h
/// has no such names, so they stay literals here (DECISIONS T62).
public enum FMotion {
    /// The ONE spring for all piece movement (card-spring): 320ms, 0.82.
    public static let card: Animation = .spring(response: 0.320, dampingFraction: 0.82)
    /// Chrome: ease-out, 150ms.
    public static let chrome: Animation = .easeOut(duration: 0.150)
    /// A pill or a die pressed, 80ms.
    public static let press: Animation = .easeOut(duration: 0.080)
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

    /// Text straight on the felt: bone ink over a dark shadow.
    func onFeltText(_ ink: Color = FColor.textPrimary) -> some View {
        self.foregroundStyle(ink)
            .shadow(color: .black.opacity(0.6), radius: 2, y: 1)
    }
}
