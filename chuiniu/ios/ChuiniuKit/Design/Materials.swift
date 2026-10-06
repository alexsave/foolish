// Materials.swift - the study's surfaces as Swift paints them (DECISIONS I27):
// the planks (the table, under every screen), the verdigris plate (the bid
// plate and every button), the barnacle crust on a plate's corner, the bone a
// flat die is cut from, and the stage's light.
//
// NOTHING IS GENERATED HERE. Every image is baked at build time by
// chuiniu/c/tools/cn_texgen.c (`make -C chuiniu/c tex-ios`) and only opened
// here: a procedural texture made at launch once took an iMessage extension
// down on a real phone. The four files and their sizes:
//   cn_planks.jpg  1032 x 1660, 2 texels a point: the study's tile, six
//                  planks of 86 points in a running bond, 516 by 830 points,
//                  seamless, the nails baked in
//   cn_verd.png    256 square, shown at 128 points (`--tex-verd`, 128px)
//   cn_crust.png   512 x 128 RGBA, shown at 192 by 48 (`.plate .crust`)
//   cn_bone.png    128 square, shown at 64 points (`p-m-tallow`)

import ImageIO
import SwiftUI
import UIKit

public enum CnTextures {
    private final class BundleToken {}
    private static let lock = NSLock()
    private static var cache: [String: UIImage] = [:]

    /// One baked image from this framework's bundle at `scale` texels a
    /// point, decoded once a process.
    static func image(_ name: String, _ ext: String, scale: CGFloat) -> UIImage? {
        lock.lock(); defer { lock.unlock() }
        if let hit = cache[name] { return hit }
        let bundle = Bundle(for: BundleToken.self)
        // decoded here, once, on whatever thread asks (`preload`: not the main
        // thread), never lazily inside a Core Animation commit
        guard let url = bundle.url(forResource: name, withExtension: ext),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let cg = CGImageSourceCreateImageAtIndex(src, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary)
        else { return nil }
        let img = UIImage(cgImage: cg, scale: scale, orientation: .up)
        cache[name] = img
        return img
    }

    /// Decode the planks off the main thread while the drawer opens: the
    /// table's first layout then only lays the tiles.
    public static func preload() {
        DispatchQueue.global(qos: .userInitiated).async { _ = planks }
    }

    /// The planks: 516 by 830 points.
    public static var planks: UIImage? { image("cn_planks", "jpg", scale: 2) }
    /// The verdigris: 128 points square.
    public static var verd: UIImage? { image("cn_verd", "png", scale: 2) }
    /// The crust: 192 by 48 points (512 by 128 texels).
    public static var crust: UIImage? { image("cn_crust", "png", scale: 512.0 / 192.0) }
    /// The bone: 64 points square.
    public static var bone: UIImage? { image("cn_bone", "png", scale: 2) }
}

/// The planks' geometry, the study's numbers (UI.html `PLANK_W_`, `TILE_W`,
/// `tableStage`'s background-position).
public enum PlankTile {
    public static let plank: CGFloat = 86
    public static let size = CGSize(width: 516, height: 830)
    /// The tile's top sits 58 points above the stage's (`-58px`).
    public static let top: CGFloat = -58

    /// Where the tile's left edge goes so that a plank's middle runs down
    /// x = `cx` (the study's `plankCentred`): at or left of 0.
    public static func left(centredOn cx: CGFloat) -> CGFloat {
        var x = cx - plank / 2
        while x > 0 { x -= plank }
        return x.rounded()
    }

    /// The tile phase for a slab `width` wide whose centre line is `cx`.
    public static func origin(centredOn cx: CGFloat) -> CGPoint { CGPoint(x: left(centredOn: cx), y: top) }
}

/// A slab of table: the planks at one point per point of the tile, a plank's
/// middle down `centreX` (default: the slab's own centre), under the stage's
/// light. Behind every flat screen and the bubble.
public struct PlanksBackground: View {
    var light = true
    /// The study's leaning screens and its bubble lay a stage 1.9 by 2.2 of
    /// the frame from -45% and -60% (`.stagewrap .over`): the tile and the
    /// light are placed in THAT stage, so only its middle shows.
    var overdraw = false
    public init(light: Bool = true, overdraw: Bool = false) { self.light = light; self.overdraw = overdraw }

    public var body: some View {
        GeometryReader { geo in
            let W = geo.size.width, H = geo.size.height
            let stage = overdraw ? CGRect(x: -0.45 * W, y: -0.6 * H, width: 1.9 * W, height: 2.2 * H) : CGRect(x: 0, y: 0, width: W, height: H)
            let t = PlankTile.origin(centredOn: stage.width / 2)
            // the tile's origin in the frame, pulled back by whole tiles to at or above-left of (0, 0)
            let ox = stage.minX + t.x, oyRaw = stage.minY + t.y
            let o = CGPoint(x: ox - (ox / PlankTile.size.width).rounded(.up) * PlankTile.size.width,
                            y: oyRaw - (oyRaw / PlankTile.size.height).rounded(.up) * PlankTile.size.height)
            ZStack(alignment: .topLeading) {
                Ink.hold
                if let img = CnTextures.planks {
                    Image(uiImage: img)
                        .resizable(resizingMode: .tile)
                        .frame(width: W - o.x, height: H - o.y)
                        .offset(x: o.x, y: o.y)
                }
                if light {
                    StageLight()
                        .frame(width: stage.width, height: stage.height)
                        .offset(x: stage.minX, y: stage.minY)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
            .clipped()
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// The stage's light (`.stage::after`): the cold glow falling from above the
/// top edge, the vignette, and the foot a little darker.
public struct StageLight: View {
    public init() {}
    public var body: some View {
        Canvas { ctx, s in
            for g in StageLight.gradients(s) {
                ctx.drawLayer { l in
                    l.scaleBy(x: 1, y: g.ry / g.rx)
                    let c = CGPoint(x: g.center.x, y: g.center.y * g.rx / g.ry)
                    let stops = g.stops.map { Gradient.Stop(color: Color(.sRGB, red: $0.rgb.0, green: $0.rgb.1, blue: $0.rgb.2, opacity: $0.a), location: $0.t) }
                    l.fill(Path(CGRect(x: 0, y: 0, width: s.width, height: s.height * g.rx / g.ry)),
                           with: .radialGradient(Gradient(stops: stops), center: c, startRadius: 0, endRadius: g.rx))
                }
            }
            ctx.fill(Path(CGRect(origin: .zero, size: s)), with: .linearGradient(
                Gradient(stops: [.init(color: .clear, location: 0.6),
                                 .init(color: Color(.sRGB, red: 0, green: 3 / 255, blue: 3 / 255, opacity: 0.35), location: 1)]),
                startPoint: .zero, endPoint: CGPoint(x: 0, y: s.height)))
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    struct Stop { var t: Double; var rgb: (Double, Double, Double); var a: Double }
    struct Ellipse { var center: CGPoint; var rx: CGFloat; var ry: CGFloat; var stops: [Stop] }

    /// The two elliptical gradients, CSS `radial-gradient(RX RY at X Y, ...)`
    /// sized for `s`. StageView's layers read the same list.
    static func gradients(_ s: CGSize) -> [Ellipse] {
        let glow = (143.0 / 255, 251.0 / 255, 224.0 / 255), deep = (1.0 / 255, 4.0 / 255, 3.0 / 255)
        return [
            Ellipse(center: CGPoint(x: s.width * 0.5, y: -0.1 * s.height), rx: 0.95 * s.width, ry: 0.6 * s.height,
                    stops: [Stop(t: 0, rgb: glow, a: 0.17), Stop(t: 0.62, rgb: glow, a: 0)]),
            Ellipse(center: CGPoint(x: s.width * 0.5, y: 0.4 * s.height), rx: 1.3 * s.width, ry: 0.9 * s.height,
                    stops: [Stop(t: 0.42, rgb: deep, a: 0), Stop(t: 1, rgb: deep, a: 0.72)]),
        ]
    }
}

/// A paint seed's offsets into a tile, so no two plates (or dice) show the
/// same slice of it. Variety for the eye only: no game reads it.
enum PaintSeed {
    static func offset(_ seed: Int, span: CGFloat) -> CGPoint {
        let a = (Double(seed) * 0.6180339887 + 0.13).truncatingRemainder(dividingBy: 1)
        let b = (Double(seed) * 0.7548776662 + 0.57).truncatingRemainder(dividingBy: 1)
        return CGPoint(x: -CGFloat(abs(a)) * span, y: -CGFloat(abs(b)) * span)
    }

    /// A number in [0, 1) from three integers (an integer mix), for wear and
    /// jitter.
    static func unit(_ a: Int, _ b: Int, _ c: Int) -> CGFloat {
        var h = UInt32(truncatingIfNeeded: a) &* 0x8DA6_B343 ^ UInt32(truncatingIfNeeded: b) &* 0xD816_3841 ^ UInt32(truncatingIfNeeded: c) &* 0xCB1A_B31F
        h = (h ^ (h >> 15)) &* 0x2C1B_3C6D
        h = (h ^ (h >> 12)) &* 0x297A_2D39
        h ^= h >> 15
        return CGFloat(h) / 4_294_967_296
    }
}

/// The verdigris tile at `seed`'s slice, filling its frame (128 points a
/// tile, `--tex-verd` at 128px).
struct VerdFill: View {
    var seed: Int
    var body: some View {
        GeometryReader { g in
            let o = PaintSeed.offset(seed, span: 128)
            ZStack(alignment: .topLeading) {
                Ink.bronze2
                if let img = CnTextures.verd {
                    Image(uiImage: img)
                        .resizable(resizingMode: .tile)
                        .frame(width: g.size.width - o.x, height: g.size.height - o.y)
                        .offset(x: o.x, y: o.y)
                }
            }
            .frame(width: g.size.width, height: g.size.height, alignment: .topLeading)
            .clipped()
        }
    }
}

/// The barnacle crust a plate wears in its lower-left corner (`.plate .crust`:
/// 120 by 48 at left -10, bottom -14, the tile at 192 by 48 from -30, at .9,
/// masked by an ellipse 60% by 80% at 20% 90%, solid to 30% of the way).
struct PlateCrust: View {
    var body: some View {
        if let img = CnTextures.crust {
            ZStack(alignment: .topLeading) {
                Image(uiImage: img)
                    .resizable(resizingMode: .tile)
                    .frame(width: 150, height: 48)
                    .offset(x: -30)
            }
            .frame(width: 120, height: 48, alignment: .topLeading)
            .clipped()
            .mask {
                Canvas { ctx, s in
                    let rx = 0.6 * s.width, ry = 0.8 * s.height
                    ctx.scaleBy(x: 1, y: ry / rx)
                    ctx.fill(Path(CGRect(x: 0, y: 0, width: s.width, height: s.height * rx / ry)),
                             with: .radialGradient(Gradient(stops: [.init(color: .black, location: 0.3), .init(color: .clear, location: 1)]),
                                                   center: CGPoint(x: 0.2 * s.width, y: 0.9 * s.height * rx / ry), startRadius: 0, endRadius: rx))
                }
            }
            .opacity(0.9)
        }
    }
}

/// One rivet (`.plate .rivet`): 5 points, lit at 35% 35%, a dark ring.
struct Rivet: View {
    var body: some View {
        Circle()
            // CSS's circle at 35% 35% reaches the far corner, 4.6 points; the dark at 70% of that
            .fill(RadialGradient(stops: [.init(color: Color(hex: 0x9A8C66), location: 0),
                                         .init(color: Color(hex: 0x2A2416), location: 0.7)],
                                 center: UnitPoint(x: 0.35, y: 0.35), startRadius: 0, endRadius: 4.6))
            .frame(width: 5, height: 5)
            .overlay(Circle().stroke(Color.black.opacity(0.5), lineWidth: 1).padding(-0.5))
    }
}

/// The plate's metal and its edges, shared by the bid plate and every plank
/// button (`.plate` / `.plank`): the verdigris, a tint (lit top, shaded foot),
/// the top highlight, the bottom shade, the outer hairline, the bevel ring
/// `ring` points in, and the drop shadow under it all.
struct PlateMetal<Tint: View>: View {
    var seed: Int
    var corner: CGFloat = 5
    var ring: CGFloat = 3
    var base: Color = Ink.bronze2
    var shadow = true
    @ViewBuilder var tint: () -> Tint

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: corner, style: .circular)
        ZStack {
            base
            VerdFill(seed: seed)
            tint()
        }
        .clipShape(shape)
        // inset 0 1px 0 rgba(255,240,200,.22), inset 0 -2px 0 rgba(0,0,0,.55)
        .overlay(alignment: .top) {
            Rectangle().fill(Color(.sRGB, red: 1, green: 240 / 255, blue: 200 / 255, opacity: 0.22)).frame(height: 1)
        }
        .overlay(alignment: .bottom) {
            Rectangle().fill(Color.black.opacity(0.55)).frame(height: 2)
        }
        .clipShape(shape)
        // inset 0 0 0 1px rgba(0,0,0,.5)
        .overlay(shape.strokeBorder(Color.black.opacity(0.5), lineWidth: 1))
        // the bevel ring: inset 3px, radius 3, rgba(0,0,0,.45) with a faint lit line inside it
        .overlay(
            RoundedRectangle(cornerRadius: max(1, corner - 2), style: .circular)
                .strokeBorder(Color.black.opacity(0.45), lineWidth: 1)
                .overlay(RoundedRectangle(cornerRadius: max(1, corner - 3), style: .circular)
                    .strokeBorder(Color(.sRGB, red: 1, green: 236 / 255, blue: 190 / 255, opacity: 0.08), lineWidth: 1)
                    .padding(1))
                .padding(ring)
        )
        .shadow(color: .black.opacity(shadow ? 0.5 : 0), radius: 7, y: 6)
    }
}

/// The bronze plate's tint (`.plate::before`, `.plank`'s --tint).
struct BronzeTint: View {
    var top = 0.14, mid = 0.42, foot = 0.38
    var body: some View {
        LinearGradient(stops: [.init(color: Color(.sRGB, red: 1, green: 236 / 255, blue: 190 / 255, opacity: top), location: 0),
                               .init(color: .clear, location: mid),
                               .init(color: Color.black.opacity(foot), location: 1)],
                       startPoint: .top, endPoint: .bottom)
    }
}
