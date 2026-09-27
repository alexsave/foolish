// COPIED from pickemup/ios/PickemupKit/Design/Materials.swift at 8e216923 (itself COPIED from foolish/ios/FoolishKit/DesignSystem/FTextures.swift and Materials.swift at c3d99192) - replaced by the shared-design lift
//
// foolish's felt, always, with its dark twin in the dark scheme (DECISIONS
// T10), and the wood of the pills and planks. Pick 'Em Up's fern card back is
// dropped: dice have no back.
//
// THE IMAGES ARE BAKED, NEVER RENDERED HERE: shared/tools/textures bakes them
// (the four JPEGs in TallybonesKit/Resources are pickemup's, byte for byte;
// foolish's round-6 lesson: a procedural render on launch took an iMessage
// extension down on a real phone). This file opens a JPEG and nothing else.

import SwiftUI
import UIKit

public enum TbTextures {
    private final class BundleToken {}
    private static let lock = NSLock()
    private static var cache: [String: UIImage] = [:]

    static func image(_ name: String) -> UIImage? {
        lock.lock(); defer { lock.unlock() }
        if let hit = cache[name] { return hit }
        let bundle = Bundle(for: BundleToken.self)
        guard let url = bundle.url(forResource: name, withExtension: "jpg"),
              let data = try? Data(contentsOf: url),
              let img = UIImage(data: data, scale: 1) else { return nil }
        cache[name] = img
        return img
    }

    public static func felt(_ scheme: ColorScheme) -> UIImage? {
        image(scheme == .dark ? FeltTexture.darkResourceName : FeltTexture.classicResourceName)
    }

    public static func wood(_ scheme: ColorScheme) -> UIImage? {
        image(scheme == .dark ? WoodTexture.darkResourceName : WoodTexture.classicResourceName)
    }

    static func feltFallback(_ scheme: ColorScheme) -> Color {
        Color(hex: (scheme == .dark ? FeltTexture.Palette.dark : FeltTexture.Palette.classic).fallbackHex)
    }

    static func woodFallback(_ scheme: ColorScheme) -> Color {
        Color(hex: (scheme == .dark ? WoodTexture.Palette.dark : WoodTexture.Palette.classic).fallbackHex)
    }
}

/// foolish's TableBackground on felt: the baked felt at ONE magnification,
/// bottom-anchored (so the drawer's collapse shows a slice of the same
/// picture rather than zooming it), under the vignette whose radii are
/// fractions of the diagonal (.32 black, .16 in the dark scheme).
public struct FeltBackground: View {
    @Environment(\.colorScheme) private var scheme
    public init() {}

    public var body: some View {
        ZStack {
            TbTextures.feltFallback(scheme)
            GeometryReader { geo in
                if let img = TbTextures.felt(scheme) {
                    let w = img.size.width * FeltTexture.pointsPerTexel
                    let h = img.size.height * FeltTexture.pointsPerTexel
                    let cover = max(1, geo.size.width / w, geo.size.height / h)
                    Image(uiImage: img)
                        .interpolation(.high)
                        .resizable()
                        .frame(width: w * cover, height: h * cover)
                        .frame(width: geo.size.width, height: geo.size.height, alignment: .bottom)
                        .clipped()
                        .contentShape(Rectangle())
                }
            }
            GeometryReader { geo in
                let diagonal = hypot(geo.size.width, geo.size.height)
                RadialGradient(colors: [.clear, .black.opacity(scheme == .dark ? 0.16 : 0.32)],
                               center: .center, startRadius: diagonal * 0.078, endRadius: diagonal * 0.682)
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
    }
}

/// foolish's WoodFill: one texel per point, never tiled or stretched, and the
/// hit region clipped to the frame (round 39: a clip hides the paint, not the
/// touch).
public struct WoodFill: View {
    @Environment(\.colorScheme) private var scheme
    public init() {}

    public var body: some View {
        TbTextures.woodFallback(scheme)
            .overlay {
                if let img = TbTextures.wood(scheme) {
                    Image(uiImage: img)
                        .interpolation(.high)
                        .resizable()
                        .frame(width: img.size.width * WoodTexture.pointsPerTexel,
                               height: img.size.height * WoodTexture.pointsPerTexel)
                }
            }
            .clipped()
            .contentShape(Rectangle())
    }
}
