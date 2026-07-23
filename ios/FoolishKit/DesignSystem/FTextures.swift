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
// DARK MODE plugs in HERE. Add `Palette.dark` in WoolTexture/WoodTexture, add a
// `dark` case to `Variant` with its own resource name, bake both, and make the
// two accessors take a Variant chosen from the environment's colorScheme. No
// caller outside this file names a file or a colour, so that change stops here.

import SwiftUI
import UIKit

public enum FTextures {

    /// Which baked palette to load. One today; `dark` is the next one (see the
    /// file header). Kept as a type rather than a bool so a third look (a
    /// seasonal table, say) is an added case and not a second flag.
    public enum Variant: String {
        case classic
    }

    /// The woven wool, at 1 texel = 1 point of the image's own coordinate space
    /// (see `WoolTexture.pointsPerTexel` for the one magnification every wool
    /// surface then applies). Nil only if the resource is missing from the
    /// bundle, in which case every wool surface shows its flat fallback colour.
    public static let wool: UIImage? = load(WoolTexture.resourceName)

    /// The wood grain, same contract (`WoodTexture.pointsPerTexel`).
    public static let wood: UIImage? = load(WoodTexture.resourceName)

    /// Load a baked texture out of FoolishKit's own bundle.
    ///
    /// `scale: 1` deliberately: the baked images carry no @2x/@3x suffix and are
    /// not meant to. Their size in POINTS is their size in texels, which is what
    /// makes `pointsPerTexel` the single honest scale knob — a UIImage that
    /// silently reported itself at 1/3 size would put a device-dependent factor
    /// back into a number whose whole job is to be device-independent.
    ///
    /// A `static let` is the cache: lazy, thread-safe, once per process. The
    /// bitmap itself is decoded lazily by CoreGraphics on first draw.
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
