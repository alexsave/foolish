// TextureStore.swift — owns the three procedural materials for the whole app.
// Generates each once (off the main thread), caches the PNG to the Caches
// directory keyed by the per-install seed, and publishes the CGImages. Injected
// at the app root as an EnvironmentObject and also reachable as `.shared` for the
// few non-View call sites (e.g. card-back rendering helpers).
//
// Resolutions are tuned for a phone: the wool weave scale is tied to the pixel
// grid (like the web's 4K note), so it renders large enough to look woven, not
// blocky, then displays scaled-to-fill. Wood is one plank sampled per button.
// The fern is small (card-back sized) and shared by every back.

import SwiftUI
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

@MainActor
public final class TextureStore: ObservableObject {
    public static let shared = TextureStore()

    @Published public private(set) var wool: CGImage?
    @Published public private(set) var wood: CGImage?
    @Published public private(set) var fern: CGImage?

    /// True until all three materials are ready (first launch only; later launches
    /// load from disk near-instantly).
    @Published public private(set) var ready = false

    private var started = false

    public init() {}

    /// Kick off generation/loading. Idempotent — safe to call from onAppear.
    public func warm() {
        guard !started else { return }
        started = true
        let seed = TextureSeed.value
        Task.detached(priority: .userInitiated) {
            let wool = Self.loadOrMake("wool-\(seed)") { ProceduralTextures.wool(1400, 2400, seed: seed) }
            await MainActor.run { self.wool = wool }
            let wood = Self.loadOrMake("wood-\(seed)") { ProceduralTextures.wood(1024, 512, seed: seed) }
            await MainActor.run { self.wood = wood }
            let fern = Self.loadOrMake("fern-\(seed)") { ProceduralTextures.fern(360, 500, seed: seed) }
            await MainActor.run { self.fern = fern; self.ready = true }
        }
    }

    // MARK: - disk cache

    nonisolated private static func cacheURL(_ key: String) -> URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("FoolishTextures", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("\(key).png")
    }

    nonisolated private static func loadOrMake(_ key: String, _ make: () -> CGImage?) -> CGImage? {
        let url = cacheURL(key)
        if let src = CGImageSourceCreateWithURL(url as CFURL, nil),
           let img = CGImageSourceCreateImageAtIndex(src, 0, nil) {
            return img
        }
        guard let img = make() else { return nil }
        if let dst = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) {
            CGImageDestinationAddImage(dst, img, nil)
            CGImageDestinationFinalize(dst)
        }
        return img
    }
}
