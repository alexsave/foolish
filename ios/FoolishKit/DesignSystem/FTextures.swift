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

    /// Which baked look to load: a colour scheme crossed with a table material.
    /// Kept as a type rather than a pair of bools so a third look (a seasonal
    /// table, say) is an added case and not a third flag.
    public enum Variant: String {
        case classic
        case dark
        case felt
        case feltDark

        /// THE mapping from the environment to a baked look, and the only one.
        /// Views read `@Environment(\.colorScheme)` and pass it here; anything
        /// else (a UIKit trait, a second read of the preference) would be a
        /// second answer to the same question.
        ///
        /// The MATERIAL is folded in here rather than at the call sites, and
        /// that is deliberate: every table surface in the app already funnels
        /// through this initializer, so felt reaches the board, the compact
        /// drawer, the lobby, the settings sheet and the message-bubble preview
        /// without one of those being able to forget. Views re-render on the
        /// change because they observe `FPrefs` (see FPrefs.swift); this is just
        /// where the current answer is read.
        ///
        /// Unknown future schemes fall to the light look on purpose: it is the
        /// one every surface's text treatments were tuned against, so an
        /// unrecognised scheme lands somewhere legible rather than half-dark.
        public init(_ scheme: ColorScheme) {
            let dark = scheme == .dark
            switch FPrefs.storedTable {
            case .wool: self = dark ? .dark : .classic
            case .felt: self = dark ? .feltDark : .felt
            }
        }

        /// Is this look a felt table? The wood and the fern do not change with
        /// the material - only the table does - so the two accessors below split
        /// on this rather than on the case list.
        var isFelt: Bool { self == .felt || self == .feltDark }
        var isDark: Bool { self == .dark || self == .feltDark }
    }

    // MARK: - Palettes
    //
    // The variant → palette mapping lives HERE and not in WoolTexture /
    // WoodTexture because those two files are compiled by the BUILD-TIME macOS
    // tool (ios/Tools/regenerate_textures.sh), which has no SwiftUI and so
    // cannot see `Variant`. Keeping the mapping on this side of that line is
    // what lets the generators stay one file each, shared by both.

    /// The flat colour a TABLE surface sits on before its texture is drawn, and
    /// all it shows if the resource is ever missing.
    ///
    /// A colour rather than a palette, because the two materials have different
    /// `Palette` TYPES (a weave is described by fibre passes, a baize by cloud
    /// and grain — see FeltTexture's header for why one cannot be a palette of
    /// the other) and `fallbackHex` is the only thing any caller wanted.
    public static func tableFallbackHex(_ variant: Variant) -> UInt32 {
        switch variant {
        case .classic:  return WoolTexture.Palette.classic.fallbackHex
        case .dark:     return WoolTexture.darkPalette.fallbackHex
        case .felt:     return FeltTexture.Palette.classic.fallbackHex
        case .feltDark: return FeltTexture.Palette.dark.fallbackHex
        }
    }

    /// The wood palette a variant wears. The controls are wood on both tables —
    /// only the table changes material — so this splits on scheme alone.
    public static func woodPalette(_ variant: Variant) -> WoodTexture.Palette {
        variant.isDark ? .dark : .classic
    }

    // MARK: - The images

    /// THE table surface — the wool weave or the green baize, whichever the
    /// player chose — at 1 texel = 1 point of the image's own coordinate space
    /// (see `WoolTexture.pointsPerTexel` for the one magnification every table
    /// surface then applies; the felt shares it by construction, so switching
    /// material cannot change the scale). Nil only if the resource is missing
    /// from the bundle, in which case the surface shows `tableFallbackHex`.
    public static func table(_ variant: Variant) -> UIImage? {
        #if DEBUG
        // DEV ONLY: a `dev.felt` file in the App Group names a JPEG beside it,
        // which stands in for the baked baize. It exists so a set of candidate
        // felts can be judged AT REAL DEVICE SCALE on a phone - the only scale
        // that settles a question like "this looks pixelated" - without a
        // rebuild per candidate. Never compiled into Release, and it only ever
        // affects the felt (the wool has no override).
        if variant.isFelt, let img = devFeltOverride() { return img }
        #endif
        return Cache.image(tableResourceName(variant))
    }

    /// The wood grain, same contract (`WoodTexture.pointsPerTexel`).
    public static func wood(_ variant: Variant) -> UIImage? {
        Cache.image(variant.isDark ? WoodTexture.darkResourceName
                                   : WoodTexture.classicResourceName)
    }

    /// The fern card back (FernCardBack) - one image, scheme-independent (the
    /// back is always the black fern in both light and dark). Nil only if the
    /// resource is missing, in which case FCard.back falls back to flat black.
    public static var fernBack: UIImage? { Cache.image(FernCardBack.resourceName) }

    /// The cache: lazy, thread-safe, and — the part that matters on an
    /// extension's memory budget — a variant nobody looks at is never opened at
    /// all. A dictionary built up front would decode both looks on the first
    /// draw of either.
    ///
    /// ROUND 16 made it PURGEABLE, which is the whole reason it is a dictionary
    /// behind a lock rather than the seven `static let`s it used to be. Measured
    /// (FoolishTests/MemoryProfileTests): every bake resident is ~17.9 MB of
    /// decoded bitmap, and a session that only ever draws one table wears ~7.5
    /// of it — but a player who toggles scheme or material in Settings pays for
    /// every variant they have looked at, for the life of the process, on a
    /// memory budget an iMessage extension cannot grow. A `static let` cannot be
    /// given back; this can, and `purge(keeping:)` does exactly that when iOS
    /// says it needs the room.
    ///
    /// Giving one back costs almost nothing to undo: `load` is "open a file,
    /// hand ~600KB of JPEG to UIImage", the same work the first draw did, and
    /// the accessors below call it again on the next body evaluation. That is
    /// the trade this makes — a hitch under memory pressure instead of a kill.
    private enum Cache {
        private static let lock = NSLock()
        private static var images: [String: UIImage] = [:]

        static func image(_ name: String) -> UIImage? {
            lock.lock()
            if let hit = images[name] { lock.unlock(); return hit }
            lock.unlock()
            // Loaded OUTSIDE the lock: a decode is slow enough that holding a
            // global lock across it would serialise every surface on screen.
            // Two threads racing the same first draw both load it and the
            // second's copy is dropped, which is cheaper than the contention.
            let img = load(name)
            lock.lock()
            if let hit = images[name] { lock.unlock(); return hit }
            images[name] = img
            lock.unlock()
            return img
        }

        static func purge(keeping keep: Set<String>) {
            lock.lock()
            images = images.filter { keep.contains($0.key) }
            lock.unlock()
        }

        static var loadedNames: Set<String> {
            lock.lock(); defer { lock.unlock() }
            return Set(images.keys)
        }
    }

    /// Give back every baked texture EXCEPT the ones `variant` is drawing right
    /// now. Called from the extension's memory warning — the one moment iOS
    /// tells us it is about to start killing things, and (before round 16) the
    /// one the app ignored entirely.
    ///
    /// The current variant is kept rather than dropping everything, because the
    /// board is on screen: re-reading the file it is already drawing would be a
    /// visible hitch bought for nothing, while the variants being dropped are by
    /// definition ones nobody is looking at.
    @discardableResult
    public static func purgeUnusedTextures(keeping variant: Variant) -> Int {
        let before = Cache.loadedNames
        Cache.purge(keeping: [tableResourceName(variant),
                              variant.isDark ? WoodTexture.darkResourceName
                                             : WoodTexture.classicResourceName,
                              FernCardBack.resourceName])
        return before.subtracting(Cache.loadedNames).count
    }

    /// Which baked file a variant's TABLE is. Split out because both the
    /// accessor and the purge have to agree about it, and a purge that named a
    /// different file from the one being drawn would drop the live texture.
    private static func tableResourceName(_ variant: Variant) -> String {
        switch variant {
        case .classic:  return WoolTexture.classicResourceName
        case .dark:     return WoolTexture.darkResourceName
        case .felt:     return FeltTexture.classicResourceName
        case .feltDark: return FeltTexture.darkResourceName
        }
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

    #if DEBUG
    /// The dev felt override, re-read every call (deliberately uncached - the
    /// whole point is to swap it under a running extension).
    private static func devFeltOverride() -> UIImage? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg"),
              let name = try? String(contentsOf: dir.appendingPathComponent("dev.felt"),
                                     encoding: .utf8)
        else { return nil }
        let file = dir.appendingPathComponent(name.trimmingCharacters(in: .whitespacesAndNewlines))
        guard let data = try? Data(contentsOf: file) else { return nil }
        return UIImage(data: data, scale: 1)
    }
    #endif

    /// Anchors `Bundle(for:)` to FoolishKit rather than the host app — the
    /// extension, the standalone iMessage app and the harness all load the same
    /// framework, so this finds the resource in all three.
    private final class BundleToken {}
}
