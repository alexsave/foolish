// Materials.swift — SwiftUI surfaces over the procedural WoolTexture/WoodTexture
// generators. Both textures render off the main thread (they are heavy the first
// time, then memory/disk cached), fading in over the flat token colour so the
// board is usable instantly and never blocks on a cold cache (§IOS_PHONE_LAYOUT
// §4: "until the two generators land, FColor.table + vignette remains").

import SwiftUI
import UIKit

/// The woven-wool table background (fixed weave scale, aspect-filled) plus the
/// one allowed gradient — a subtle centre-out vignette (§5.1).
public struct WoolBackground: View {
    // Process-wide cache of the generated weave. Without it, every new
    // WoolBackground instance started with `img == nil` and faded the texture in
    // over the flat colour — so the board's wool flickered on every view reload
    // (each incoming bubble / player switch, which recreates MessagesRootView via
    // `.id`). Seeding @State from the cache shows the texture instantly, no fade,
    // after the first generation.
    private static var cached: UIImage?
    @State private var img: UIImage? = WoolBackground.cached
    public init() {}

    /// The cover-scale the wool texture needs to fill a FULL-HEIGHT stage on
    /// this device — the constant that keeps the background's magnification the
    /// same in the compact drawer as in the expanded board. Per-device, not
    /// per-view, which is the whole point: read it from the screen so a view
    /// that is only 261pt tall still draws the wool at the size it has when the
    /// board is full-screen, and simply shows less of it.
    static var screenFitScale: CGFloat {
        let screen = UIScreen.main.bounds.size
        return max(screen.width / CGFloat(WoolTexture.webCanvas.w),
                   screen.height / CGFloat(WoolTexture.webCanvas.h))
    }

    public var body: some View {
        ZStack {
            FColor.fallback   // web's beige base behind the wool (was dark-green felt)
            if let img {
                Image(uiImage: img)
                    .interpolation(.high)          // smooth the downscale (see below)
                    .resizable()
                    // NOT `.aspectRatio(.fill)`. Fill sizes the texture to cover
                    // whatever box it is in, and the extension's box changes
                    // height by 2.5x between expanded and compact — so the wool
                    // was drawn at 0.62 scale expanded and 0.24 compact, i.e.
                    // the background visibly ZOOMED on every collapse ("the
                    // expanded view is the correct zoom, use it for the
                    // collapsed view as well").
                    //
                    // So the scale is pinned to the SCREEN, not to this view:
                    // whatever cover-scale the texture would need to fill a
                    // full-height stage. Compact then shows a vertical slice of
                    // the very same picture at the very same magnification,
                    // which is what "the same zoom" means. Anchored to the
                    // BOTTOM because the drawer is: its bottom edge is pinned to
                    // the screen while its top edge moves, so bottom-anchoring
                    // keeps the visible weave still as it grows and shrinks.
                    .frame(width: CGFloat(WoolTexture.webCanvas.w) * Self.screenFitScale,
                           height: CGFloat(WoolTexture.webCanvas.h) * Self.screenFitScale)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .clipped()
                    .transition(.opacity)
            }
            // The vignette scales with the SURFACE, not in absolute points (note
            // 3). The old fixed 80/700 radii were tuned against a full-screen
            // phone board (~1026pt diagonal), where 80pt is a small clear centre
            // and 700pt lands the falloff near the edges. The iMessage
            // extension's stage is barely half that (375x554, ~669pt diagonal),
            // so those same radii put the surface almost entirely inside the
            // ramp: it never reached the clear centre and read as uniformly
            // muddy rather than as a vignette. That is exactly why the tester
            // found the message BUBBLE's wool better than the live extension's -
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
        .task {
            guard img == nil else { return }
            // Render ABOVE the display resolution and let SwiftUI downscale it —
            // the website does the same (4K → phone), which anti-aliases the
            // weave into fine threads instead of the blocky upscale of a small
            // texture. Heavy the first time, then cached forever (memory + the
            // static above, so reloads don't re-fade).
            let image = await Task.detached(priority: .userInitiated) {
                WoolTexture.image(w: WoolTexture.webCanvas.w, h: WoolTexture.webCanvas.h)
            }.value
            WoolBackground.cached = image
            withAnimation(.easeOut(duration: 0.4)) { img = image }
        }
    }
}

/// A wood-grain fill at ONE fixed points-per-texel scale (round-5 B2, the
/// wood-grain half) over a wood-toned fallback. Use behind controls via
/// `.woodSurface(cornerRadius:)`.
public struct WoodFill: View {
    @State private var img: UIImage?
    public init() {}

    /// The swatch canvas — the generator's own small default, deliberately.
    ///
    /// This was briefly raised to 384×288 so one fixed-size patch could cover
    /// the tallest wood surface (the 8-row game-over plank). That is a REAL
    /// cost, not a free constant: `WoodTexture.render` is a per-pixel loop over
    /// 576 grain columns × every row × 25 inner iterations × a 40px span, so
    /// tripling the canvas tripled a ~70M-operation render to ~170M. It
    /// finished fine on a Mac's simulator and blew the budget on a real phone —
    /// an iMessage extension is memory- and watchdog-capped far below an app,
    /// and the extension came up as a dark, empty panel on device while the
    /// simulator looked perfect. Constant grain does not require a giant
    /// swatch; it requires TILING one (see `body`).
    private static let texSize = CGSize(width: 300, height: 120)

    public var body: some View {
        // The FALLBACK COLOUR is the only thing that sizes this view, and that
        // is load-bearing. A `Color` is infinitely flexible — it takes whatever
        // its container proposes. A fixed-size swatch drawn as a ZStack sibling
        // instead made every wood surface demand the swatch's own width: the
        // game-over plank overflowed the board, got centred, and its
        // `.clipShape` sliced the rank column off the left edge — B2's exact
        // symptom, reintroduced by the cure. As an `.overlay` the grain is
        // painted and clipped but contributes NOTHING to layout.
        Color(hex: 0x5A2412)          // dark wood, close to the texture's own 70/14/9 base
            .overlay {
                if let img {
                    // TILED, not stretched and not a fixed patch (round-5 B2:
                    // "the wood grains should be the same size everywhere, just
                    // maybe smaller or larger wood chunks depending on button
                    // need"). `.tile` repeats the swatch at its intrinsic size,
                    // so one texel is one point on every surface: a 96×40 action
                    // pill shows a small piece of exactly the same grain a
                    // full-width button or a tall plank shows more of. The
                    // earlier `.aspectRatio(.fill)` is what B2 blamed — fill
                    // lets the destination's HEIGHT dictate the width, so a
                    // taller plank got proportionally giant grain.
                    Image(uiImage: img)
                        .resizable(resizingMode: .tile)
                        .transition(.opacity)
                }
            }
            .clipped()
            .task {
                guard img == nil else { return }
                let image = await Task.detached(priority: .userInitiated) {
                    WoodTexture.image(w: Int(WoodFill.texSize.width), h: Int(WoodFill.texSize.height))
                }.value
                withAnimation(.easeOut(duration: 0.3)) { img = image }
            }
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
