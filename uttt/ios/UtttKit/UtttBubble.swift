import CUttt
import CoreGraphics
import Messages
import UIKit

/// The transcript bubble: one image, one line of text, both baked at insert.
///
/// THIS IS THE FRAME NOBODY CHOSE. `MSMessageTemplateLayout` renders its image
/// at exactly 300 by 195 points - landscape, aspect 1.54 - and a board is
/// square. UI.html gives it 181; the kernel gives it 170 so the grid's main
/// lines stop on the frame (uttt_bubble), and the rest is two lines: a
/// headline, and the block the opponent has been sent to.
///
/// AND IT IS BAKED. Every device in the thread shows the one image the sender
/// drew and reads the one caption under it, so neither may say "you": the
/// image names the side to play by its drawn mark ("<O> to play"), because
/// docs/UI.html's "Your move" is false on the sender's own copy.
///
/// AND THE CAPTION NAMES NOBODY, though UI.html 01 and 05 write "Alex". An
/// extension has a participant UUID and no name, and the documented way to
/// get one - "$<uuid>" in a caption, which Messages is meant to swap for the
/// person's name - was tried on iOS 27 in the simulator (2026-09-22): the raw
/// "$FEACEE0B-..." showed in the draft, the sent bubble, the incoming twin
/// and the conversation list. The kernel can already word both sentences
/// around a name (uttt_say_by); what is missing is proof on two real phones
/// that Messages substitutes it, and until then a UUID in the transcript is
/// far worse than no name.
///
/// Nothing here computes a coordinate. The frame is split in `uttt_draw.c` and
/// this fills in what the kernel cannot know: where a baseline falls in a font
/// it has never seen.
public enum UtttBubble {

    // MARK: what the kernel says the frame is

    /// 300 x 195. Messages' number, not ours.
    public static var size: CGSize {
        var w: Float = 0, h: Float = 0
        uti_bubble_size(&w, &h)
        return CGSize(width: CGFloat(w), height: CGFloat(h))
    }

    private static var boardBox: CGRect {
        var x: Float = 0, y: Float = 0, s: Float = 0
        uti_bubble_board(&x, &y, &s)
        return CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(s), height: CGFloat(s))
    }

    private static var textBox: CGRect {
        var x: Float = 0, y: Float = 0, w: Float = 0, h: Float = 0
        uti_bubble_text(&x, &y, &w, &h)
        return CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
    }

    private static func ink(_ line: Int32) -> UIColor {
        let c = uti_bubble_ink(line)
        return UIColor(red:   CGFloat((c >> 24) & 0xff) / 255,
                       green: CGFloat((c >> 16) & 0xff) / 255,
                       blue:  CGFloat((c >>  8) & 0xff) / 255,
                       alpha: CGFloat( c        & 0xff) / 255)
    }

    private static func font(_ line: Int32) -> UIFont {
        .systemFont(ofSize: CGFloat(uti_bubble_type(line)), weight: .bold)
    }

    // MARK: what it says

    /// Every line of the bubble is the kernel's (uttt_say.h). It is BAKED:
    /// one bitmap and one caption, identical on every device in the thread,
    /// so none of it says "you" and none of it names a person - the
    /// transcript already says who did it, by which side the bubble sits on.

    /// Two words after a drawn mark. The one thing a glance needs.
    public static var headline: String { Uttt.say(.bubbleHeadline) }

    /// The place, in blue, under the headline.
    public static var place: String { Uttt.say(.bubblePlace) }

    /// One line, truncating, and it names nobody (see the type's comment).
    public static var caption: String { Uttt.say(.caption) }

    // MARK: the image

    /// The resident game, drawn into the 300x195 frame.
    ///
    /// Baked at THREE TIMES, always. The sender's device renders it once and
    /// every other device in the thread gets that bitmap, so rendering at the
    /// sender's own scale would hand a 2x phone's bubble to a 3x one and the
    /// ink would be soft for the rest of the game.
    public static func image() -> UIImage {
        let frame = CGRect(origin: .zero, size: size)
        let board = boardBox
        let last = Uttt.plyCount > 0 ? Uttt.move(at: Uttt.plyCount - 1) : -1
        /* AN EMPTY BOARD GETS NO WASH. `uti_active()` says 9 - anywhere - and
         * mid-game that is right, so the whole sheet goes yellow and the
         * animation expands it from the last block. On an INVITATION it is
         * wrong twice over: nobody is on move, and a board tinted corner to
         * corner stops reading as "you may go anywhere" and starts reading as
         * a different piece of paper sitting next to the white one the text is
         * on. The design document's invitation board carries no active block
         * for exactly this reason. */
        let active = Uttt.plyCount > 0 ? Int(uti_active()) : -1

        let fmt = UIGraphicsImageRendererFormat()
        fmt.scale = 3
        fmt.opaque = true
        return UIGraphicsImageRenderer(size: frame.size, format: fmt).image { rc in
            let cg = rc.cgContext

            if let sheet = paper(width: Int(frame.width), height: Int(frame.height)) {
                UIImage(cgImage: sheet).draw(in: frame)
            } else {
                UIColor(red: 0.969, green: 0.965, blue: 0.949, alpha: 1).setFill()
                cg.fill(frame)
            }

            /* The kernel draws in a unit square and the board is 170 points in
             * the corner the kernel knows nothing about, so the transform goes
             * on once here rather than into ten thousand multiplications. */
            cg.saveGState()
            cg.translateBy(x: board.minX, y: board.minY)
            Uttt.fill(Uttt.bubbleBoardPolys(active: active, last: last),
                      into: cg, side: board.width)
            cg.restoreGState()

            draw(mark: Uttt.bubbleMark, headline: headline, place: place,
                 in: textBox, into: cg)
        }
    }

    /// The layout Messages inserts. The caption is the only text outside the
    /// image, and it is the only text that can name a person.
    public static func layout() -> MSMessageTemplateLayout {
        let l = MSMessageTemplateLayout()
        l.image = image()
        l.caption = caption
        return l
    }

    // MARK: the two lines

    /* ABOUT NINETY-FIVE POINTS. That is what is left of the 300 once the
     * board has had its 170 and the padding its gutters, and "bottom middle"
     * in bold 18 is well over that - so the headline gets one line and the
     * place is allowed to wrap onto two, as it does in UI.html option 02. Every block name is two short words, so it always breaks
     * cleanly and nothing ever truncates; the truncating line is the caption,
     * which is the one carrying a name it did not choose. */
    private static func draw(mark: Uttt.Mark, headline: String, place: String,
                             in box: CGRect, into cg: CGContext) {
        let one = NSMutableParagraphStyle()
        one.lineBreakMode = .byTruncatingTail
        let wrap = NSMutableParagraphStyle()
        wrap.lineBreakMode = .byWordWrapping

        let hAttr: [NSAttributedString.Key: Any] = [
            .font: font(0), .foregroundColor: ink(0), .paragraphStyle: one,
        ]
        let pFont = font(1)
        let pAttr: [NSAttributedString.Key: Any] = [
            .font: pFont, .foregroundColor: ink(1), .paragraphStyle: wrap,
        ]

        let wide = CGSize(width: box.width, height: .greatestFiniteMagnitude)
        let opts: NSStringDrawingOptions = [.usesLineFragmentOrigin]
        let hH = ceil((headline as NSString)
            .boundingRect(with: wide, options: opts, attributes: hAttr, context: nil).height)
        /* AN EMPTY PLACE LINE TAKES NO ROOM AT ALL - not a blank line's worth.
         * Measuring "" still returns a line height, and the headline would sit
         * that far above the middle of a frame whose whole job is to look
         * composed. */
        let pH = place.isEmpty ? 0 : min(ceil((place as NSString)
            .boundingRect(with: wide, options: opts, attributes: pAttr, context: nil).height),
                     ceil(pFont.lineHeight * 2))
        let lead0 = place.isEmpty ? 0 : CGFloat(uti_bubble_lead())

        /* The two lines are ONE block and the block is centred, which is not
         * the same as centring either line - the board's middle and the
         * headline's baseline have to look related or the frame reads as two
         * pictures that arrived together. */
        let lead = lead0
        var y = box.minY + (box.height - (hH + lead + pH)) / 2
        /* THE MARK SITS ON THE CAP HEIGHT, not the line box, or it reads as
         * a separate object hanging below the words. It is the same pen as
         * the board's, so it is the only name either side has. */
        let hFont = font(0)
        var hx = box.minX
        if mark == .x || mark == .o {
            let side = ceil(hFont.capHeight * 1.8)
            let base = y + hFont.ascender
            let mid = base - hFont.capHeight / 2
            cg.saveGState()
            cg.translateBy(x: hx - side * 0.06, y: mid - side / 2)
            fill(Uttt.mark(mark, seed: Uttt.seed &+ 4), into: cg, side: side)
            cg.restoreGState()
            hx += side * 0.94 + 4
        }
        (headline as NSString).draw(
            with: CGRect(x: hx, y: y, width: box.maxX - hx, height: hH),
            options: opts, attributes: hAttr, context: nil)
        y += hH + lead
        (place as NSString).draw(
            with: CGRect(x: box.minX, y: y, width: box.width, height: pH),
            options: opts, attributes: pAttr, context: nil)
    }

    private static func fill(_ polys: [Uttt.Poly], into cg: CGContext, side: CGFloat) {
        for p in polys where p.points.count > 1 {
            cg.setFillColor(p.color)
            cg.beginPath()
            cg.move(to: CGPoint(x: p.points[0].x * side, y: p.points[0].y * side))
            for q in p.points.dropFirst() { cg.addLine(to: CGPoint(x: q.x * side, y: q.y * side)) }
            cg.closePath()
            cg.fillPath()
        }
    }

    // MARK: the sheet

    /* UtttPaper only makes squares and this frame is not one. Same kernel
     * call, same grain, asked for 300 by 195 so the fibres are not stretched
     * into the only part of the app where they would be. */
    private static var sheet: (w: Int, h: Int, img: CGImage)?

    private static func paper(width: Int, height: Int) -> CGImage? {
        if let s = sheet, s.w == width, s.h == height { return s.img }
        guard width > 0, height > 0 else { return nil }
        var px = [UInt8](repeating: 0, count: width * height * 4)
        px.withUnsafeMutableBufferPointer {
            uti_paper($0.baseAddress, Int32(width), Int32(height))
        }
        guard let provider = CGDataProvider(data: Data(px) as CFData),
              let img = CGImage(width: width, height: height,
                                bitsPerComponent: 8, bitsPerPixel: 32,
                                bytesPerRow: width * 4,
                                space: CGColorSpaceCreateDeviceRGB(),
                                bitmapInfo: CGBitmapInfo(rawValue:
                                    CGImageAlphaInfo.premultipliedLast.rawValue),
                                provider: provider, decode: nil,
                                shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        sheet = (width, height, img)
        return img
    }
}
