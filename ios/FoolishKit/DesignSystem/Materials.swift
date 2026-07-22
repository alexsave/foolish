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

    public var body: some View {
        ZStack {
            FColor.fallback   // web's beige base behind the wool (was dark-green felt)
            if let img {
                Image(uiImage: img)
                    .interpolation(.high)          // smooth the downscale (see below)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
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

/// A wood-grain fill (stretched swatch) over a wood-toned fallback. Use behind
/// controls via `.woodSurface(cornerRadius:)`.
public struct WoodFill: View {
    @State private var img: UIImage?
    public init() {}

    public var body: some View {
        ZStack {
            Color(hex: 0x5A2412)   // dark wood fallback while the swatch renders
            if let img {
                Image(uiImage: img).resizable().aspectRatio(contentMode: .fill)
                    .frame(maxWidth: .infinity, maxHeight: .infinity).clipped()
                    .transition(.opacity)
            }
        }
        .task {
            guard img == nil else { return }
            let image = await Task.detached(priority: .userInitiated) { WoodTexture.image() }.value
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
