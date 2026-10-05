// NameDecal.swift - A SEAT'S NAME IS A PICTURE ON THE TABLE (cn_stage.h, package N).
// The kernel lays each name flat on the planks inside its scene, so a cup in
// front of a name hides it and a cup's shadow falls on it, as on a real table.
// The letters stay the host's (a nickname in any script, in IM Fell English
// SC): this file draws a seat's name and its turn bar into a premultiplied
// RGBA bitmap, three texels a point, and hands it to the stage (`TableStage.
// name`) only when it changes. WHERE it lies is the kernel's (cn_lay's anchor
// and name_how, cn_stage_name_rect); the bitmap is the block (the letters and
// the bar under them, as the old layers laid them out) with CN_STAGE_NAME_HALO
// points of room on every side for the shadow and the glow.

import UIKit

/// A name's bitmap as the stage takes it (cn_api_stage_name): premultiplied
/// RGBA, `w` by `h` texels, `wPt` by `hPt` points.
public struct NameBitmap: Equatable {
    public let rgba: [UInt8]
    public let w: Int
    public let h: Int
    public let wPt: CGFloat
    public let hPt: CGFloat
}

enum NameDecal {
    /// `.t-name`: the small caps at 14, 12 on a short board; tracked .14em.
    static func nameFont(short: Bool) -> UIFont { FType.uiSC(short ? 12 : 14) }

    /// A name's ink (`.t-name`, `.t-name.dim`): bright on its turn, dim
    /// otherwise, the glow for the winner, dimmer still once out.
    static func nameInk(_ n: StageName) -> UIColor {
        let ink = n.won ? Ink.glow : (n.isTurn ? Ink.ink : Ink.inkdim)
        return UIColor(ink.opacity(n.alive ? 1 : 0.6))
    }

    /// Everything a name's picture is drawn from: two equal looks are the same
    /// bitmap, so the stage is handed a name only when its look changes.
    struct Look: Equatable {
        var name: String
        var ink: UIColor
        var bar: Bool
        var short: Bool
        /// a far seat's (CN_NAME_BOX): its bar is 36 wide, mine 44
        var box: Bool
    }

    static func look(_ n: StageName, short: Bool, how: Int) -> Look {
        Look(name: n.name, ink: nameInk(n), bar: n.isTurn, short: short, box: how == CN_NAME_BOX)
    }

    static let gap: CGFloat = 3, barH: CGFloat = 2

    /// The block as the old layers laid it out: the letters' box (their width
    /// and 8, at most 150; the font's line high) over a 3-point gap and the
    /// 2-point bar. The stage sets this block on the anchor (cn_stage_name_rect)
    /// and the reveal's stamp hangs under it.
    static func block(_ name: String, short: Bool) -> (textW: CGFloat, textH: CGFloat) {
        let font = nameFont(short: short)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .kern: FType.nameTracking(font.pointSize)]
        return (min(150, ceil((name as NSString).size(withAttributes: attrs).width) + 8), ceil(font.lineHeight))
    }

    /// The texels a point: three, or fewer for a name so long its bitmap would
    /// pass the stage's maxima.
    static func density(wPt: CGFloat, hPt: CGFloat) -> CGFloat {
        min(3, CGFloat(CN_STAGE_NAME_W_MAX) / wPt, CGFloat(CN_STAGE_NAME_H_MAX) / hPt)
    }

    /// The name drawn: the letters centred in their box (the old layer's black
    /// shadow, a point down), the turn's bar centred under them (`.turn`: the
    /// glow, 2 tall, haloed), the halo round it all.
    static func render(_ look: Look) -> NameBitmap? {
        let halo = CGFloat(CN_STAGE_NAME_HALO)
        let (textW, textH) = block(look.name, short: look.short)
        let wPt = textW + 2 * halo, hPt = textH + gap + barH + 2 * halo
        let k = density(wPt: wPt, hPt: hPt)
        let w = min(CN_STAGE_NAME_W_MAX, Int((wPt * k).rounded(.up))), h = min(CN_STAGE_NAME_H_MAX, Int((hPt * k).rounded(.up)))
        guard w >= 2, h >= 2 else { return nil }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = false
        format.preferredRange = .standard
        let picture = UIGraphicsImageRenderer(size: CGSize(width: w, height: h), format: format).image { ctx in
            let cg = ctx.cgContext
            cg.scaleBy(x: CGFloat(w) / wPt, y: CGFloat(h) / hPt)
            let font = nameFont(short: look.short)
            let shadow = NSShadow()
            shadow.shadowColor = UIColor.black
            shadow.shadowOffset = CGSize(width: 0, height: 1)
            shadow.shadowBlurRadius = 1
            let style = NSMutableParagraphStyle()
            style.alignment = .center
            style.lineBreakMode = .byTruncatingTail
            let text = NSAttributedString(string: look.name, attributes: [
                .font: font, .kern: FType.nameTracking(font.pointSize), .foregroundColor: look.ink,
                .shadow: shadow, .paragraphStyle: style,
            ])
            text.draw(with: CGRect(x: halo, y: halo, width: textW, height: textH), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine], context: nil)
            guard look.bar else { return }
            let barW: CGFloat = look.box ? 36 : 44
            let bar = UIBezierPath(roundedRect: CGRect(x: halo + textW / 2 - barW / 2, y: halo + textH + gap, width: barW, height: barH), cornerRadius: 1)
            // a shadow's blur is in the bitmap's texels, not its points
            cg.setShadow(offset: .zero, blur: 5 * k, color: UIColor(Ink.glow).withAlphaComponent(0.9).cgColor)
            UIColor(Ink.glow).setFill()
            bar.fill()
        }
        guard let image = picture.cgImage, let rgba = premultipliedRGBA(image, w: w, h: h) else { return nil }
        return NameBitmap(rgba: rgba, w: w, h: h, wPt: wPt, hPt: hPt)
    }

    /// The picture's texels in the stage's order: R G B A, premultiplied, rows
    /// top down, no padding.
    static func premultipliedRGBA(_ image: CGImage, w: Int, h: Int) -> [UInt8]? {
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let cg = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: space,
                                 bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)
        else { return nil }
        cg.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        guard let copy = cg.makeImage(), let data = copy.dataProvider?.data as Data?, data.count >= w * h * 4 else { return nil }
        return [UInt8](data.prefix(w * h * 4))
    }
}

/// What was handed to the stage, a seat at a time: a name is drawn and handed
/// over only when its look changes (a new name, the turn, the winner, out, a
/// board that turned short), and taken away when its seat goes.
@MainActor
final class NameDecals {
    private(set) var given: [Int: NameDecal.Look] = [:]
    /// how many times the stage was handed a name (tests)
    private(set) var handed = 0

    /// The names as the screen wants them on the begun table. Whether the
    /// stage was handed anything (then the next frame draws it).
    @discardableResult
    func update(_ names: [StageName], hud: CnStageHudSnap?, stage: TableStage) -> Bool {
        guard let h = hud, h.kind != StageScreen.bubble.rawValue else { return false }
        var want: [Int: NameDecal.Look] = [:]
        for n in names where n.seat < h.seats && h.nameHow.indices.contains(n.seat) {
            want[n.seat] = NameDecal.look(n, short: h.shortBoard != 0, how: h.nameHow[n.seat])
        }
        var changed = false
        for seat in Set(want.keys).union(given.keys).sorted() where want[seat] != given[seat] {
            stage.name(seat: seat, bitmap: want[seat].flatMap(NameDecal.render))
            given[seat] = want[seat]
            handed += 1
            changed = true
        }
        return changed
    }
}
