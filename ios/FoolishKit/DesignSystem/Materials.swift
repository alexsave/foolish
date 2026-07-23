// Materials.swift — SwiftUI surfaces over the BAKED wool/wood textures
// (§IOS_PHONE_LAYOUT §4). The generators still live in WoolTexture.swift /
// WoodTexture.swift as the source of truth, but they run at build time now
// (ios/Tools/regenerate_textures.sh); these views just load an image through
// FTextures and draw it at ONE fixed magnification.
//
// That is why nothing here has a @State image, a `.task`, or a fade-in any
// more: there is no cold cache to fade over. The texture is present on the
// first frame, on the first launch, in the extension, in ImageRenderer, and in
// a Preview alike — which also deletes a whole class of round-5 bug (the wool
// flickering on every view reload because a fresh instance started at nil).

import SwiftUI
import UIKit

/// The weave itself, at THE magnification (`WoolTexture.pointsPerTexel`), with
/// no clipping and no vignette. EVERY wool surface draws this and then decides
/// how big a window it wants onto it — which is round-6 #14 in one sentence:
/// "keep the threads the same size visually no matter the view".
///
/// It is a fixed-size view on purpose. Callers wrap it in their own frame and
/// `.clipped()`; a smaller surface shows LESS weave, never smaller weave.
public struct WoolWeave: View {
    public init() {}

    public var body: some View {
        if let img = FTextures.wool {
            Image(uiImage: img)
                // The weave is magnified ~2.3x on a 3x screen (0.775pt/texel x
                // 3), so the interpolation is doing real work smoothing threads
                // rather than anti-aliasing a downscale.
                .interpolation(.high)
                .resizable()
                .frame(width: img.size.width * WoolTexture.pointsPerTexel,
                       height: img.size.height * WoolTexture.pointsPerTexel)
        }
    }
}

/// The woven-wool table background plus the one allowed gradient — a subtle
/// centre-out vignette (§5.1).
public struct WoolBackground: View {
    public init() {}

    public var body: some View {
        ZStack {
            FColor.fallback   // web's beige base behind the wool (was dark-green felt)

            // Bottom-anchored, and that is load-bearing for the extension: the
            // drawer's bottom edge is pinned to the screen while its top edge
            // moves, so bottom-anchoring keeps the visible weave STILL as the
            // surface grows and shrinks. Compact then shows a vertical slice of
            // the very same picture at the very same magnification, which is
            // what "the expanded view is the correct zoom, use it for the
            // collapsed view as well" means.
            //
            // Note there is no `.aspectRatio(.fill)` anywhere near this. Fill
            // sizes the texture to cover whatever box it is in, and the
            // extension's box changes height by 2.5x between expanded and
            // compact — so the wool was drawn at 0.62 scale expanded and 0.24
            // compact, i.e. the background visibly ZOOMED on every collapse.
            WoolWeave()
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                .clipped()

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
            GeometryReader { geo in
                let diagonal = hypot(geo.size.width, geo.size.height)
                RadialGradient(colors: [.clear, .black.opacity(0.32)],
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
    public init() {}

    public var body: some View {
        // The FALLBACK COLOUR is the only thing that sizes this view, and that
        // is load-bearing. A `Color` is infinitely flexible — it takes whatever
        // its container proposes. A fixed-size swatch drawn as a ZStack sibling
        // instead made every wood surface demand the swatch's own width: the
        // game-over plank overflowed the board, got centred, and its
        // `.clipShape` sliced the rank column off the left edge — B2's exact
        // symptom, reintroduced by the cure. As an `.overlay` the grain is
        // painted and clipped but contributes NOTHING to layout.
        Color(hex: WoodTexture.Palette.classic.fallbackHex)
            .overlay {
                if let img = FTextures.wood {
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
