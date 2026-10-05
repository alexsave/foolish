// Tokens.swift - the study's palette, type and motion (chuiniu/docs/UI.html:
// the product's :root block, the Palette and Type tabs). DECISIONS I27.
//
// The palette is the drowned one sampled from the five frames: a hold of
// near-black teal, bone, bronze gone to verdigris, one cold aquamarine glow
// for whose turn it is and what counts, and blood for the call. Nothing here
// is warm except the bronze under the verdigris and the blood.
//
// THE TYPE is one family, IM Fell English, roman and small caps (SIL OFL,
// chuiniu/c/tools/fonts, copied into the bundle by `make tex-ios`). The faces
// live in ChuiniuKit's own bundle, not the extension's, so UIAppFonts (which
// reads only the main bundle) cannot name them: they are registered once a
// process, at first use, with Core Text for the process.

import CoreText
import SwiftUI
import UIKit

/// The study's palette (UI.html :root), by the study's names.
public enum Ink {
    public static let abyss   = Color(hex: 0x020805)
    public static let hold    = Color(hex: 0x0E1916)
    public static let hold2   = Color(hex: 0x162926)
    public static let sea     = Color(hex: 0x0B2E32)
    public static let bone    = Color(hex: 0xD3DCC3)
    public static let bone2   = Color(hex: 0xE6EAD8)
    public static let bronze2 = Color(hex: 0x433B22)
    public static let verd2   = Color(hex: 0x5E8A7A)
    /// The one cold light: the turn bar, the counting ring, a lit verb.
    public static let glow    = Color(hex: 0x8FFBE0)
    /// The call: Liar's plate, the loser's stamp, "loses a die".
    public static let blood   = Color(hex: 0xC04A33)
    public static let ink     = Color(hex: 0xE2E7D4)
    public static let inkdim  = Color(hex: 0x8EA39A)
    /// The 1's pip: the deep red locked in foolish's round 44.
    public static let wild    = Color(hex: 0x8B1A1A)
    /// Under the planks, where nothing is drawn (the study's `.scr.peekB`).
    public static let glass   = Color(hex: 0x050807)
}

/// 4pt grid.
public enum FSpace {
    public static let xs: CGFloat = 4
    public static let s: CGFloat = 8
    public static let m: CGFloat = 12
    public static let l: CGFloat = 16
    public static let xl: CGFloat = 24
}

/// The two faces, by PostScript name, registered from this framework's bundle.
public enum FType {
    public static let serifName = "IM_FELL_English_Roman"
    public static let scName = "IM_FELL_English_SC"
    static let files = ["IMFeENrm28P", "IMFeENsc28P"]

    private final class BundleToken {}
    /// Once a process: both faces registered (or already there). True when
    /// both resolve by name afterwards.
    public static let registered: Bool = {
        let bundle = Bundle(for: BundleToken.self)
        let urls = files.compactMap { bundle.url(forResource: $0, withExtension: "ttf") }
        for url in urls { CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil) }
        return UIFont(name: serifName, size: 12) != nil && UIFont(name: scName, size: 12) != nil
    }()

    /// The roman (bids, numbers, running lines). Fixed size: the table is laid
    /// out in points and does not follow Dynamic Type.
    public static func serif(_ size: CGFloat) -> Font {
        _ = registered
        return .custom(serifName, fixedSize: size)
    }
    /// The small caps (names, verbs, stamps).
    public static func sc(_ size: CGFloat) -> Font {
        _ = registered
        return .custom(scName, fixedSize: size)
    }
    public static func uiSerif(_ size: CGFloat) -> UIFont {
        _ = registered
        return UIFont(name: serifName, size: size) ?? .systemFont(ofSize: size)
    }
    public static func uiSC(_ size: CGFloat) -> UIFont {
        _ = registered
        return UIFont(name: scName, size: size) ?? .systemFont(ofSize: size)
    }
    /// `.t-name`'s tracking, .14em.
    public static func nameTracking(_ size: CGFloat) -> CGFloat { size * 0.14 }
}

/// Motion tokens. The ONE spring for chrome that moves, a fade, a press.
public enum FMotion {
    public static let card: Animation = .spring(response: 0.320, dampingFraction: 0.82)
    public static let chrome: Animation = .easeOut(duration: 0.150)
    /// `.plank:active`, transform .08s ease-out.
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
    /// Type straight on the planks (`.t-ask`, `.t-out`, `.t-name`):
    /// `text-shadow: 0 1px 2px rgba(0,0,0,.7)`.
    func onPlanks(_ ink: Color = Ink.ink) -> some View {
        foregroundStyle(ink).shadow(color: .black.opacity(0.75), radius: 1.2, y: 1)
    }

    /// The bid's serif (`.t-bid`, `.t-num`): bone, a hard black line under
    /// it and the faint cold glow round it.
    func bidInk() -> some View {
        foregroundStyle(Ink.bone2)
            .shadow(color: Ink.glow.opacity(0.16), radius: 9)
            .shadow(color: .black, radius: 0, y: 1)
    }
}
