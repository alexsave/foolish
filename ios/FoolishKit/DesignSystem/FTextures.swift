// FTextures.swift — THE loader for the baked wool/wood surfaces, and the only
// thing the shipping app knows about either texture.
//
// Round-6 #16. Both materials used to be GENERATED at launch: WoolTexture wove
// ~2.4M brush strokes into an 8.3MB buffer, WoodTexture blended ~70M-166M
// pixels, each behind a memory+disk cache that only ever helped the SECOND
// launch. The launch that paid for them was the iMessage extension's, which
// iOS memory- and watchdog-caps far below an app: on a real phone the extension
// came up as a dark, empty panel while the simulator (no cap, no watchdog)
// looked perfect. Round-5's interim answer was to tile a small wood swatch;
// round-6 rejected that ("tiling isn't an option").
//
// So the generators moved to BUILD time (ios/Tools/regenerate_textures.sh) and
// this file loads their output. The extension's cold-start texture work is now:
// open a file, hand ~600KB of JPEG to UIImage. ZERO procedural pixels, zero
// scratch buffers, no disk cache to warm, and identical behaviour on a fresh
// install and on the thousandth launch.
//
// DARK MODE plugged in HERE, exactly as the round-6 note above predicted it
// would: `Variant` grew a `dark` case, the generators grew dark `Palette`s, the
// bake grew three more files, and the two accessors became functions of a
// Variant that `Variant(_ scheme:)` derives from the environment. NOTHING
// outside this file names a resource or a hex — `Materials.swift` asks for the
// variant's image and the variant's fallback colour, and that is the whole of
// the app's knowledge about which look is on screen.

import SwiftUI
import UIKit

public enum FTextures {

    /// Which baked palette to load. Kept as a type rather than a bool so a third
    /// look (a seasonal table, say) is an added case and not a second flag.
    ///
    /// The green-vs-navy dark wool is deliberately NOT a case here. It is not a
    /// third THEME - it is one theme's accent, and modelling it as a case would
    /// put a choice with no UI behind it into every switch over this type.
    /// `WoolTexture.darkAccent` owns that choice; see its doc.
    public enum Variant: String {
        case classic
        case dark

        /// THE mapping from SwiftUI's colour scheme to a baked look, and the
        /// only one. Views read `@Environment(\.colorScheme)` and pass it here;
        /// anything else (a UIKit trait, a stored preference) would be a second
        /// answer to the same question.
        ///
        /// Unknown future schemes fall to `classic` on purpose: the light look
        /// is the one every surface's text treatments were tuned against, so an
        /// unrecognised scheme lands somewhere legible rather than somewhere
        /// half-dark.
        public init(_ scheme: ColorScheme) {
            self = (scheme == .dark) ? .dark : .classic
        }
    }

    // MARK: - Palettes
    //
    // The variant → palette mapping lives HERE and not in WoolTexture /
    // WoodTexture because those two files are compiled by the BUILD-TIME macOS
    // tool (ios/Tools/regenerate_textures.sh), which has no SwiftUI and so
    // cannot see `Variant`. Keeping the mapping on this side of that line is
    // what lets the generators stay one file each, shared by both.

    /// The wool palette a variant wears. The dark one is `WoolTexture`'s own
    /// `darkAccent` choice, so flipping green↔navy needs no edit here.
    public static func woolPalette(_ variant: Variant) -> WoolTexture.Palette {
        switch variant {
        case .classic: return .classic
        case .dark:    return WoolTexture.darkPalette
        }
    }

    /// The wood palette a variant wears.
    public static func woodPalette(_ variant: Variant) -> WoodTexture.Palette {
        switch variant {
        case .classic: return .classic
        case .dark:    return .dark
        }
    }

    // MARK: - The images

    /// The woven wool, at 1 texel = 1 point of the image's own coordinate space
    /// (see `WoolTexture.pointsPerTexel` for the one magnification every wool
    /// surface then applies). Nil only if the resource is missing from the
    /// bundle, in which case every wool surface shows the variant's flat
    /// fallback colour.
    public static func wool(_ variant: Variant) -> UIImage? {
        switch variant {
        case .classic: return Cache.woolClassic
        case .dark:    return Cache.woolDark
        }
    }

    /// The wood grain, same contract (`WoodTexture.pointsPerTexel`).
    public static func wood(_ variant: Variant) -> UIImage? {
        switch variant {
        case .classic: return Cache.woodClassic
        case .dark:    return Cache.woodDark
        }
    }

    /// One `static let` per baked image IS the cache: lazy, thread-safe, once
    /// per process, and — the part that matters on an extension's memory
    /// budget — a variant nobody looks at is never opened at all. A dictionary
    /// built up front would decode both looks on the first draw of either.
    private enum Cache {
        static let woolClassic = load(WoolTexture.classicResourceName)
        static let woolDark    = load(WoolTexture.darkResourceName)
        static let woodClassic = load(WoodTexture.classicResourceName)
        static let woodDark    = load(WoodTexture.darkResourceName)
    }

    /// Load a baked texture out of FoolishKit's own bundle.
    ///
    /// `scale: 1` deliberately: the baked images carry no @2x/@3x suffix and are
    /// not meant to. Their size in POINTS is their size in texels, which is what
    /// makes `pointsPerTexel` the single honest scale knob — a UIImage that
    /// silently reported itself at 1/3 size would put a device-dependent factor
    /// back into a number whose whole job is to be device-independent.
    ///
    /// The bitmap itself is decoded lazily by CoreGraphics on first draw.
    private static func load(_ name: String) -> UIImage? {
        let bundle = Bundle(for: BundleToken.self)
        guard let url = bundle.url(forResource: name, withExtension: "jpg"),
              let data = try? Data(contentsOf: url) else { return nil }
        return UIImage(data: data, scale: 1)
    }

    /// Anchors `Bundle(for:)` to FoolishKit rather than the host app — the
    /// extension, the standalone iMessage app and the harness all load the same
    /// framework, so this finds the resource in all three.
    private final class BundleToken {}
}
