// FeltTexture.swift — the green casino-baize table surface, the alternative to
// the wool weave (owner, round 12: "Distracting wool?? Have felt texture green
// casino table option in settings. Not quite solid, but with slightly lighter
// and darker like felt patterns").
//
// Same contract as WoolTexture.swift in every respect that matters, because the
// two are interchangeable at the point of use (FTextures picks one, Materials
// draws whichever it gets): a `Palette` holds every colour, `bakes` lists what
// the build-time tool renders, and `pointsPerTexel` is THE magnification so a
// felt table has the same physical grain size on every screen.
//
// THIS FILE NO LONGER RUNS IN THE SHIPPING APP — same rule as the wool, and for
// the same reason. It is executed at BUILD time by ios/Tools/GenerateTextures
// .swift, which bakes one image per `bakes` entry into FoolishKit/Resources/.
//
// TO CHANGE THE LOOK: edit `render` / `Palette` here, then run
//   ios/Tools/regenerate_textures.sh
// and commit the regenerated images.
//
// Deliberately UIKit-free (CoreGraphics only) so the macOS build-time tool can
// compile this exact file — one generator, no port to drift.
//
// WHY IT IS NOT A WOOL PALETTE. The wool generator is a woven structure: a weft
// pass, a warp pass and a plaid modulation, all of which read as THREADS. Felt
// is the opposite material — non-woven matted fibre, no threads, no repeat — so
// no choice of wool palette produces it (the plaid chequer alone would give the
// game away). This is a different generator, ~40 lines, and that is the honest
// cost of a second material.

import CoreGraphics
import Foundation

public enum FeltTexture {

    // MARK: - Palette

    /// Every colour and amplitude the baize uses.
    ///
    /// The look is one base green plus two disturbances at very different
    /// scales, which is what "not quite solid, but with slightly lighter and
    /// darker" describes physically: a slow CLOUD (matted fibre density varying
    /// over a couple of centimetres) and a fine GRAIN (the individual fibres).
    /// Neither is large enough to read as a pattern; together they stop the
    /// surface reading as a flat fill.
    public struct Palette {
        /// The baize green everything modulates around.
        public let baseR, baseG, baseB: Double
        /// Peak luminance swing of the slow cloud, in 0-255 units.
        public let cloud: Double
        /// Peak luminance swing of the fibre grain, in 0-255 units.
        public let grain: Double
        /// Texels per grain cell. MUST BE > 1, and that is the whole point.
        ///
        /// The first version drew the grain as per-TEXEL white noise, which
        /// looked correct in the bake and pixelated on the phone: a texel is
        /// magnified to ~2.3 device pixels (`pointsPerTexel` 0.775 x 3), so
        /// noise whose finest feature IS one texel renders as hard-edged 2-3px
        /// blocks - the texture showing you its own grid. Band-limiting it to a
        /// couple of texels puts every edge inside the magnification, where the
        /// resampler can smooth it, and the surface reads as nap instead of as
        /// pixels. Felt is matted fibre; it has no feature as sharp as a pixel.
        public let grainScale: Double
        /// How the swing is split across channels. Lit baize goes lighter AND a
        /// touch yellower (the fibre catches the light), shadowed baize goes
        /// darker and bluer — a flat grey swing on all three channels reads as
        /// dust on the surface rather than as the surface itself.
        public let tiltR, tiltG, tiltB: Double
        /// Texels per cloud cell, across and down. NOT equal: baize has a nap,
        /// and a slightly stretched cloud reads as brushed fibre where a square
        /// one reads as blobs.
        public let cloudX, cloudY: Double
        /// The flat colour a felt surface sits on before the texture is drawn
        /// (and all it shows if the resource is ever missing). Same contract as
        /// WoolTexture.Palette.fallbackHex: close to the texture's own average,
        /// so a failure is dull rather than wrong. NOT a generator input.
        public let fallbackHex: UInt32

        public init(baseR: Double, baseG: Double, baseB: Double,
                    cloud: Double, grain: Double, grainScale: Double,
                    tiltR: Double, tiltG: Double, tiltB: Double,
                    cloudX: Double, cloudY: Double, fallbackHex: UInt32) {
            self.baseR = baseR; self.baseG = baseG; self.baseB = baseB
            self.cloud = cloud; self.grain = grain; self.grainScale = grainScale
            self.tiltR = tiltR; self.tiltG = tiltG; self.tiltB = tiltB
            self.cloudX = cloudX; self.cloudY = cloudY
            self.fallbackHex = fallbackHex
        }

        /// The light-mode baize: a card-room green, deep enough that white card
        /// faces and the black role marks both hold on it.
        ///
        /// Amplitudes are the owner's pick from six candidates shot on device
        /// ("E, nearly solid"): the least texture that is still not a flat fill.
        /// The light twin of that pick keeps the same RATIOS against its own
        /// base rather than copying the dark numbers - a brighter surface shows
        /// a given swing more, so equal numbers would not read as equal texture.
        public static let classic = Palette(
            baseR: 40, baseG: 102, baseB: 70,
            cloud: 6.5, grain: 3.0, grainScale: 3.0,
            tiltR: 0.85, tiltG: 1.0, tiltB: 0.72,
            cloudX: 62, cloudY: 48,
            fallbackHex: 0x286646)

        /// The dark-mode baize. Not the light one dimmed: the cloud and grain
        /// keep almost their full amplitude while the base drops, because a dark
        /// surface has less room to go darker and a proportionally-scaled
        /// texture on it flattens into a slab (the same trap WoolTexture's dark
        /// palette documents for the weave).
        public static let dark = Palette(
            baseR: 20, baseG: 56, baseB: 39,
            cloud: 5, grain: 2.5, grainScale: 3.0,
            tiltR: 0.85, tiltG: 1.0, tiltB: 0.72,
            cloudX: 62, cloudY: 48,
            fallbackHex: 0x143827)
    }

    // MARK: - The shipped swatch

    /// The baked size, in texels. Unlike the wool — whose generator is written
    /// in the pixels of the web's 1920x1080 canvas and therefore has to render
    /// big and crop — felt has no inherited constants, so it is rendered at
    /// exactly the size that ships. Same texel count as `WoolTexture
    /// .shippedCrop`, so both materials cover the same stage at the same
    /// magnification and switching between them cannot change the scale.
    public static let renderCanvas = (w: 592, h: 1280)

    /// THE magnification, shared with the wool by construction: a table is a
    /// table, and a player who switches material must not see the board zoom.
    public static var pointsPerTexel: CGFloat { WoolTexture.pointsPerTexel }

    public static let classicResourceName = "felt-classic"
    public static let darkResourceName = "felt-dark"

    /// Every baize the build-time tool bakes, as (file base name, palette).
    public static let bakes: [(name: String, palette: Palette)] = [
        (classicResourceName, .classic),
        (darkResourceName, .dark),
    ]

    // MARK: - The generator

    /// A hashed lattice value in 0..1. Deterministic and stateless — no RNG to
    /// seed, so two machines bake byte-identical images and a re-bake after an
    /// unrelated edit is a no-op in git.
    @inline(__always)
    private static func lattice(_ x: Int, _ y: Int, _ salt: UInt64) -> Double {
        var h = UInt64(bitPattern: Int64(x)) &* 0x9E3779B97F4A7C15
        h ^= UInt64(bitPattern: Int64(y)) &* 0xC2B2AE3D27D4EB4F
        h ^= salt &* 0x165667B19E3779F9
        h ^= h >> 29; h = h &* 0xBF58476D1CE4E5B9
        h ^= h >> 32; h = h &* 0x94D049BB133111EB
        h ^= h >> 29
        return Double(h >> 40) / Double(1 << 24)
    }

    /// Smoothed value noise over that lattice (bilinear with a smoothstep fade,
    /// so the cloud has no lattice-aligned creases in it).
    @inline(__always)
    private static func valueNoise(_ x: Double, _ y: Double, _ salt: UInt64) -> Double {
        let xi = Int(floor(x)), yi = Int(floor(y))
        let fx = x - Double(xi), fy = y - Double(yi)
        let ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy)
        let a = lattice(xi, yi, salt),     b = lattice(xi + 1, yi, salt)
        let c = lattice(xi, yi + 1, salt), d = lattice(xi + 1, yi + 1, salt)
        return (a * (1 - ux) + b * ux) * (1 - uy) + (c * (1 - ux) + d * ux) * uy
    }

    /// Render the baize at `w x h` texels. Deterministic. Pure CoreGraphics so
    /// the build-time tool can call it on macOS.
    ///
    /// BUILD-TIME ONLY. Nothing in the shipping app may call this; the app loads
    /// the baked image through `FTextures`.
    public static func renderCGImage(w: Int, h: Int,
                                     palette p: Palette = .classic) -> CGImage? {
        var data = [UInt8](repeating: 255, count: w * h * 4)

        data.withUnsafeMutableBufferPointer { buf in
            guard let d = buf.baseAddress else { return }
            for y in 0..<h {
                let cy = Double(y) / p.cloudY
                for x in 0..<w {
                    let cx = Double(x) / p.cloudX
                    // Four octaves of cloud. Three is visibly banded at this
                    // amplitude; five costs time for a change under a JPEG
                    // quantisation step.
                    var cloud = 0.0, amp = 1.0, freq = 1.0, norm = 0.0
                    for o in 0..<4 {
                        cloud += valueNoise(cx * freq, cy * freq, UInt64(o) &+ 1) * amp
                        norm += amp
                        amp *= 0.5
                        freq *= 2.07      // not exactly 2: octaves that share a
                                          // lattice alignment pile up into grid
                                          // artefacts at the coarse end
                    }
                    cloud = cloud / norm - 0.5

                    // Smoothed, not per-texel: see `grainScale`. Two octaves so
                    // the nap has some variation in it rather than one uniform
                    // ripple - the second is finer and half the weight.
                    let gx = Double(x) / p.grainScale, gy = Double(y) / p.grainScale
                    let grain = (valueNoise(gx, gy, 0x5EED) * 2
                                 + valueNoise(gx * 2.13, gy * 2.13, 0xBEEF)) / 3 - 0.5
                    let v = cloud * 2 * p.cloud + grain * 2 * p.grain

                    let i = (y * w + x) * 4
                    d[i]     = clamp(p.baseR + v * p.tiltR)
                    d[i + 1] = clamp(p.baseG + v * p.tiltG)
                    d[i + 2] = clamp(p.baseB + v * p.tiltB)
                    d[i + 3] = 255
                }
            }
        }

        guard let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        var pixels = data
        return pixels.withUnsafeMutableBytes { raw -> CGImage? in
            guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h,
                                      bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return nil }
            return ctx.makeImage()
        }
    }

    @inline(__always)
    private static func clamp(_ v: Double) -> UInt8 {
        UInt8(max(0, min(255, v.rounded())))
    }
}
