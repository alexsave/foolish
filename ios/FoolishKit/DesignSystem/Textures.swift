// Textures.swift — SwiftUI surfaces that paint the procedural materials from
// TextureStore. Kept tiny and declarative so every screen/component just drops a
// `WoolBackground()`, a `.woodSurface(seed:)`, or a `FernBack()`.

import SwiftUI

// MARK: - wool background

/// Full-bleed woven-wool background with a warm vignette so content reads. Shows
/// the flat fallback color until the texture finishes generating on first launch.
public struct WoolBackground: View {
    @ObservedObject private var store = TextureStore.shared
    public init() {}
    public var body: some View {
        GeometryReader { geo in
            ZStack {
                FColor.table
                if let wool = store.wool {
                    Image(decorative: wool, scale: 1, orientation: .up)
                        .resizable()
                        .scaledToFill()
                        .frame(width: geo.size.width, height: geo.size.height)
                        .clipped()
                }
                // Warm vignette: darkens the edges so cards/panels pop (web
                // --color-vignette over the weave).
                RadialGradient(colors: [.clear, FColor.vignette],
                               center: .center,
                               startRadius: geo.size.height * 0.18,
                               endRadius: geo.size.height * 0.72)
                    .ignoresSafeArea()
            }
            .ignoresSafeArea()
        }
        .ignoresSafeArea()
    }
}

// MARK: - wood surface (buttons, planks)

/// A wooden-plank fill sampled from the shared wood plank at a per-seed offset,
/// with the raised-edge inset lighting the web buttons use.
public struct WoodSurface: View {
    @ObservedObject private var store = TextureStore.shared
    let seed: Double
    let cornerRadius: CGFloat
    public init(seed: Double = 0.5, cornerRadius: CGFloat = FRadius.button) {
        self.seed = seed
        self.cornerRadius = cornerRadius
    }
    public var body: some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        return ZStack {
            FColor.wood
            if let wood = store.wood {
                Image(decorative: wood, scale: 1, orientation: .up)
                    .resizable()
                    .scaledToFill()
                    // Per-seed pan so adjacent buttons show different grain.
                    .scaleEffect(1.6)
                    .offset(x: (seed - 0.5) * 220, y: (seed * 1.7 - 0.5) * 90)
            }
        }
        .clipShape(shape)
        .overlay( // raised-plank lighting: top highlight + bottom shadow
            shape.stroke(Color.white.opacity(0.16), lineWidth: 1)
                .blur(radius: 0.5)
                .mask(LinearGradient(colors: [.white, .clear], startPoint: .top, endPoint: .bottom))
        )
        .overlay(shape.strokeBorder(FColor.woodDark, lineWidth: 2))
        .shadow(color: .black.opacity(0.4), radius: 3, x: 0, y: 2)
    }
}

public extension View {
    /// Fill `self`'s background with a wooden plank.
    func woodSurface(seed: Double = 0.5, cornerRadius: CGFloat = FRadius.button) -> some View {
        background(WoodSurface(seed: seed, cornerRadius: cornerRadius))
    }
}

// MARK: - fern card back

/// The shared fern card back. Rotated 90° so the frond runs across the card
/// (matching the web's landscape fern), on black with a dark-red edge.
public struct FernBack: View {
    @ObservedObject private var store = TextureStore.shared
    var cornerRadius: CGFloat = FRadius.card
    public init(cornerRadius: CGFloat = FRadius.card) { self.cornerRadius = cornerRadius }
    public var body: some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        ZStack {
            Color.black
            if let fern = store.fern {
                Image(decorative: fern, scale: 1, orientation: .up)
                    .resizable()
                    .scaledToFill()
                    .rotationEffect(.degrees(90))
            }
        }
        .clipShape(shape)
        .overlay(shape.strokeBorder(Color(hex: 0x8B0000), lineWidth: 1))
    }
}
