import CUttt
import CoreGraphics
import Messages
import UIKit

/// The transcript bubble: one image, one line of text, both baked at insert.
///
/// THIS IS THE FRAME NOBODY CHOSE. `MSMessageTemplateLayout` renders its image
/// at exactly 300 by 195 points - landscape, aspect 1.54 - and a board is
/// square. The kernel gives it 168 so the grid's main lines stop on the
/// frame (uttt_bubble). A game in play is the board alone, centred, and the
/// caption says whose turn ("O to play", owner; the tint shows where);
/// only a finished game's image has words, the winner's drawn mark and
/// "wins" over "N moves", in a column beside the board.
///
/// AND IT IS BAKED. Every device in the thread shows the one image the sender
/// drew and reads the one caption under it, so neither may say "you": the
/// winner is named by a drawn mark, because "You win" is false on the
/// loser's copy.
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

    /// The frame for the resident game (uttt_bubble): read with the snapshot,
    /// never while painting off the main thread.
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
        UtttInk.rgba(uti_bubble_ink(line))
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

    /// The resident game, drawn into the 300x195 frame at the sender's scale
    /// (`uttt_bubble_scale`): the sender's device renders it once and every
    /// other device in the thread gets that bitmap.
    public static func image(display: CGFloat) -> UIImage { image(snapshot(display: display)) }

    /// EVERYTHING THE BUBBLE NEEDS FROM THE KERNEL, read on the main thread
    /// in about a millisecond, so the picture - fourteen thousand fills at 3x,
    /// which blocked the main thread for a fifth of a second at every stage and
    /// froze the board's highlighter mid-travel - can be painted anywhere.
    public struct Snapshot: @unchecked Sendable {
        let board: Uttt.BoardPolys
        let mark: Uttt.Mark
        let markPolys: [Uttt.Poly]
        let headline: String
        let place: String
        let paper: CGImage?
        let boardBox: CGRect
        /// Zero-sized for a game in play: its image has no words.
        let textBox: CGRect
        /// Pixels a point in the bake (`uttt_bubble_scale`).
        let scale: CGFloat
    }

    /// `display` is the sender's screen scale (`traitCollection.displayScale`),
    /// read on the main thread with everything else.
    public static func snapshot(display: CGFloat) -> Snapshot {
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
        let mark = Uttt.bubbleMark
        return Snapshot(board: Uttt.bubbleBoardPolys(active: active, last: last),
                        mark: mark,
                        markPolys: (mark == .x || mark == .o) ? Uttt.mark(mark, seed: Uttt.seed &+ 4) : [],
                        headline: headline, place: place,
                        paper: paper(width: Int(size.width), height: Int(size.height)),
                        boardBox: boardBox, textBox: textBox,
                        scale: CGFloat(uti_bubble_scale(Float(display))))
    }

    /* THE SCALE THE BUBBLE IS BAKED AT is the sender's own screen, 2 to 3
     * (`uttt_bubble_scale`, owner: crispness). It was a fixed 2 (TESTFLIGHT_PLAN
     * 14): on the SE's transcript a 2x and a 3x bake are the same picture, but
     * a 3x phone shows the frame at up to 300 points, 900 pixels, and a 2x
     * bake is upscaled 1.5x there. The 3x bitmap is 2.1 MB, not 1; the stage's
     * peak on a 3x phone pays about that, and it was judged worth it. */

    /// Pure: the snapshot painted. Safe on any thread.
    ///
    /// ONE BITMAP, EIGHT BITS A CHANNEL, NO ALPHA, NO COPIES (TESTFLIGHT_PLAN
    /// 14). This was a UIGraphicsImageRenderer, which on a wide-colour screen
    /// picks an extended-range format (16 bits a channel) and whose image was
    /// then drawn through a UIImage of the paper: the paint put the stage's
    /// memory peak 12 MB over the drawer's idle (log `mem painted`). A plain
    /// sRGB context with no alpha is the bytes the image needs and no more,
    /// and `makeImage` hands the same pixels over copy-on-write.
    public static func image(_ snap: Snapshot) -> UIImage {
        let frame = CGRect(origin: .zero, size: size)
        let board = snap.boardBox
        let scale = snap.scale
        let pw = Int((frame.width * scale).rounded()), ph = Int((frame.height * scale).rounded())
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let cg = CGContext(data: nil, width: pw, height: ph, bitsPerComponent: 8,
                                 bytesPerRow: 0, space: space,
                                 bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                                     | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return UIImage() }
        /* TOP-LEFT ORIGIN, POINTS: the kernel's coordinates and UIKit's text
         * drawing both mean this. */
        cg.translateBy(x: 0, y: CGFloat(ph))
        cg.scaleBy(x: scale, y: -scale)

        if let sheet = snap.paper {
            /* A CGImage draws bottom-up: flipped back for this one draw. */
            cg.saveGState()
            cg.translateBy(x: 0, y: frame.height)
            cg.scaleBy(x: 1, y: -1)
            cg.interpolationQuality = .high
            cg.draw(sheet, in: frame)
            cg.restoreGState()
        } else {
            cg.setFillColor(UIColor(red: 0.969, green: 0.965, blue: 0.949, alpha: 1).cgColor)
            cg.fill(frame)
        }

        /* The kernel draws in a unit square and the board is 168 points
         * where uttt_bubble puts it, so the transform goes
         * on once here rather than into ten thousand multiplications. */
        cg.saveGState()
        cg.translateBy(x: board.minX, y: board.minY)
        Uttt.fill(snap.board, into: cg, side: board.width)
        cg.restoreGState()

        if snap.textBox.width > 0 {
            UIGraphicsPushContext(cg)
            draw(mark: snap.mark, markPolys: snap.markPolys,
                 headline: snap.headline, place: snap.place, in: snap.textBox, into: cg)
            UIGraphicsPopContext()
        }
        guard let img = cg.makeImage() else { return UIImage() }
        return UIImage(cgImage: img, scale: scale, orientation: .up)
    }

    /// The layout Messages inserts. The caption is the only text outside the
    /// image, and it is the only text that can name a person.
    public static func layout(display: CGFloat) -> MSMessageTemplateLayout {
        layout(image: image(display: display), caption: caption)
    }

    /// The layout around an image painted from a snapshot, with the caption
    /// read at the same moment.
    public static func layout(image: UIImage, caption: String) -> MSMessageTemplateLayout {
        let l = MSMessageTemplateLayout()
        l.image = image
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
    private static func draw(mark: Uttt.Mark, markPolys: [Uttt.Poly], headline: String, place: String,
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
            /* the middle of the lower-case words beside it ("wins", "to
             * play"), so the two are centred on each other (owner) */
            let mid = base - hFont.xHeight / 2
            cg.saveGState()
            cg.translateBy(x: hx - side * 0.06, y: mid - side / 2)
            fill(markPolys, into: cg, side: side)
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
