// Materials.swift — SwiftUI surfaces over the BAKED table/wood textures
// (§IOS_PHONE_LAYOUT §4). The generators still live in WoolTexture.swift /
// FeltTexture.swift / WoodTexture.swift as the source of truth, but they run at
// build time now (ios/Tools/regenerate_textures.sh); these views just load an
// image through FTextures and draw it at ONE fixed magnification.
//
// NOTE the table views are named for the SURFACE, not the material: since round
// 12 the player picks wool or green baize in Settings (FPrefs.table), and
// `TableWeave` draws whichever one `FTextures.Variant` resolves to. Nothing here
// knows which it got, which is the point — a material is a palette, a bake and a
// case in that enum, and no layout, vignette or text treatment changes with it.
//
// That is why nothing here has a @State image, a `.task`, or a fade-in any
// more: there is no cold cache to fade over. The texture is present on the
// first frame, on the first launch, in the extension, in ImageRenderer, and in
// a Preview alike — which also deletes a whole class of round-5 bug (the wool
// flickering on every view reload because a fresh instance started at nil).

import SwiftUI
import UIKit

/// The table surface itself, at THE magnification (`WoolTexture.pointsPerTexel`,
/// which the felt shares by construction), and no vignette. EVERY table surface
/// draws this and then decides how big a window it wants onto it — which is
/// round-6 #14 in one sentence: "keep the threads the same size visually no
/// matter the view".
///
/// It FILLS its container and clips itself, bottom-anchored: a smaller surface
/// shows LESS weave, never smaller weave. (It used to be a fixed-size view that
/// callers framed and clipped themselves; filling is the same picture at the
/// same magnification for every surface that fits inside the bake, and it is
/// what lets the cover floor below exist at all — a fixed-size view cannot know
/// it is too small.)
///
/// ROUND 16 #1, the cover floor: the bake is 592x1280 texels, i.e. 458.6 x
/// 991.6pt, and a surface BIGGER than that in either axis used to show the flat
/// fallback colour where the picture ran out ("the wool simply does not cover
/// the entire screen - the bottom was just some flat solid color"). The margin
/// was thin enough to be a coin flip on hardware that ships today: a 440pt-wide
/// Pro Max had 18pt of width to spare, and any future taller phone, an iPad, or
/// a surface that momentarily overshoots during a transition has none. So when
/// the container does not fit inside the bake, the weave is magnified by the
/// SMALLEST factor that covers it, rather than leaving a slab of flat colour.
/// Every surface in the app today fits, so this is a no-op on all of them —
/// threads stay exactly the same size everywhere, which is what round-6 #14
/// asked for; it is strictly a floor under the failure case, and it degrades to
/// slightly-larger threads instead of to a bare rectangle.
public struct TableWeave: View {
    /// Dark mode — and, since round 12, the MATERIAL — is chosen HERE, once per
    /// surface, and never below: every table view in the app draws this one, so
    /// a board cannot end up half-dark or half-felt. The
    /// message bubble pins `.light` on its whole ImageRenderer content
    /// (BubbleSnapshot), which is what keeps a sender's dark board out of a
    /// recipient's light transcript - it works because this reads the
    /// environment rather than a global.
    @Environment(\.colorScheme) private var scheme
    /// Re-render when the table MATERIAL changes (FPrefs). Without this the
    /// switch does not reach the board: these views have no stored properties,
    /// so a parent re-render hands SwiftUI a structurally identical value and it
    /// skips the body - the setting was stored, the checkmark moved, and the
    /// wool stayed. `FTextures.Variant` reads the preference; this is what makes
    /// anyone ASK it again.
    @ObservedObject private var prefs = FPrefs.shared

    public init() {}

    public var body: some View {
        GeometryReader { geo in
            if let img = FTextures.table(FTextures.Variant(scheme)) {
                let w = img.size.width * WoolTexture.pointsPerTexel
                let h = img.size.height * WoolTexture.pointsPerTexel
                // 1 = THE magnification. Above 1 only when the surface is
                // bigger than the bake (see the type's doc) — never below, so
                // no surface can ever shrink the threads.
                let cover = max(1, geo.size.width / w, geo.size.height / h)
                Image(uiImage: img)
                    // The weave is magnified ~2.3x on a 3x screen (0.775pt/texel
                    // x 3), so the interpolation is doing real work smoothing
                    // threads rather than anti-aliasing a downscale.
                    .interpolation(.high)
                    .resizable()
                    .frame(width: w * cover, height: h * cover)
                    // Bottom-anchored, and that is load-bearing for the
                    // extension — see TableBackground, which used to own this
                    // frame. The clip is here too, so no caller can forget it.
                    .frame(width: geo.size.width, height: geo.size.height, alignment: .bottom)
                    .clipped()
            }
        }
    }
}

/// The table background plus the one allowed gradient — a subtle centre-out
/// vignette (§5.1).
public struct TableBackground: View {
    @Environment(\.colorScheme) private var scheme
    /// Re-render when the table MATERIAL changes (FPrefs). Without this the
    /// switch does not reach the board: these views have no stored properties,
    /// so a parent re-render hands SwiftUI a structurally identical value and it
    /// skips the body - the setting was stored, the checkmark moved, and the
    /// wool stayed. `FTextures.Variant` reads the preference; this is what makes
    /// anyone ASK it again.
    @ObservedObject private var prefs = FPrefs.shared

    public init() {}

    public var body: some View {
        ZStack {
            // The flat colour behind the texture - the web's beige in light
            // mode, the dark walnut in dark mode, the baize green on felt. It
            // comes off the MATERIAL's own palette, not off `FColor.fallback`,
            // because it has exactly one job: be the colour that texture
            // averages to, so a missing resource or an uncovered edge reads as
            // a duller board rather than as a beige slab under a dark one.
            Color(hex: FTextures.tableFallbackHex(FTextures.Variant(scheme)))

            // Bottom-anchored (inside TableWeave now), and that is load-bearing
            // for the extension: the drawer's bottom edge is pinned to the
            // screen while its top edge moves, so bottom-anchoring keeps the
            // visible weave STILL as the surface grows and shrinks. Compact then
            // shows a vertical slice of the very same picture at the very same
            // magnification, which is what "the expanded view is the correct
            // zoom, use it for the collapsed view as well" means.
            //
            // Note there is no `.aspectRatio(.fill)` anywhere near this. Fill
            // sizes the texture to cover whatever box it is in, and the
            // extension's box changes height by 2.5x between expanded and
            // compact — so the wool was drawn at 0.62 scale expanded and 0.24
            // compact, i.e. the background visibly ZOOMED on every collapse.
            // TableWeave's round-16 cover floor is NOT that: it never scales a
            // surface the bake already covers, so it cannot move with the box.
            TableWeave()

            // The vignette scales with the SURFACE, not in absolute points. The
            // old fixed 80/700 radii were tuned against a full-screen phone
            // board (~1026pt diagonal), where 80pt is a small clear centre and
            // 700pt lands the falloff near the edges. The iMessage extension's
            // stage is barely half that (375x554, ~669pt diagonal), so those
            // same radii put the surface almost entirely inside the ramp: it
            // never reached the clear centre and read as uniformly muddy rather
            // than as a vignette. That is exactly why the tester found the
            // message BUBBLE's wool better than the live extension's -
            // BubbleSnapshot draws no vignette at all (see its doc), so it was
            // the only place the weave showed at full contrast.
            //
            // Expressed as fractions of the diagonal, both surfaces get the same
            // LOOK: the fractions below are the old radii over that full-screen
            // diagonal, so a full-screen board is unchanged to the eye and every
            // smaller surface gets a proportionally smaller falloff.
            //
            // Dark mode halves the falloff. The vignette's job is to sit the
            // bright light weave down at the edges; on the dark weave (a third
            // the average luminance) the same 0.32 black lands on a surface
            // that has nowhere left to go, so the corners crush to flat black
            // and the board reads as a hole rather than as a table. 0.16 keeps
            // the same SHAPE - the geometry below is untouched - at a strength
            // proportional to what the surface can actually absorb.
            GeometryReader { geo in
                let diagonal = hypot(geo.size.width, geo.size.height)
                RadialGradient(colors: [.clear, .black.opacity(scheme == .dark ? 0.16 : 0.32)],
                               center: .center,
                               startRadius: diagonal * 0.078,
                               endRadius: diagonal * 0.682)
            }
        }
        .ignoresSafeArea()
    }
}

/// A wood-grain fill at ONE fixed points-per-texel scale (round-5 B2, the
/// wood-grain half) over a wood-toned fallback. Use behind controls via
/// `.woodSurface(cornerRadius:)`.
public struct WoodFill: View {
    @Environment(\.colorScheme) private var scheme

    public init() {}

    public var body: some View {
        let variant = FTextures.Variant(scheme)
        // The FALLBACK COLOUR is the only thing that sizes this view, and that
        // is load-bearing. A `Color` is infinitely flexible — it takes whatever
        // its container proposes. A fixed-size swatch drawn as a ZStack sibling
        // instead made every wood surface demand the swatch's own width: the
        // game-over plank overflowed the board, got centred, and its
        // `.clipShape` sliced the rank column off the left edge — B2's exact
        // symptom, reintroduced by the cure. As an `.overlay` the grain is
        // painted and clipped but contributes NOTHING to layout.
        Color(hex: FTextures.woodPalette(variant).fallbackHex)
            .overlay {
                if let img = FTextures.wood(variant) {
                    // One texel, one point, on every surface — not tiled, not
                    // stretched, not aspect-filled (round-5 B2: "the wood
                    // grains should be the same size everywhere, just maybe
                    // smaller or larger wood chunks depending on button need").
                    // The swatch is baked large enough to contain the biggest
                    // wood surface in the app (WoodTexture.renderCanvas), so a
                    // 96x40 action pill shows a small centre piece of exactly
                    // the grain the tall game-over plank shows more of, and
                    // NOTHING repeats — round-6 #16 struck the tiling that was
                    // round-5's stopgap.
                    Image(uiImage: img)
                        .interpolation(.high)
                        .resizable()
                        .frame(width: img.size.width * WoodTexture.pointsPerTexel,
                               height: img.size.height * WoodTexture.pointsPerTexel)
                }
            }
            .clipped()
    }
}

public extension View {
    /// Put a wood-grain surface behind this view, clipped to a rounded rect with
    /// a thin darker rim (the physical-button look from the web's WoodTexture).
    func woodSurface(cornerRadius: CGFloat = FRadius.card) -> some View {
        self.background(
            WoodFill()
                .clipShape(RoundedRectangle(cornerRadius: cornerRadius))
                .overlay(
                    RoundedRectangle(cornerRadius: cornerRadius)
                        .strokeBorder(Color.black.opacity(0.35), lineWidth: 1)
                )
        )
    }
}
