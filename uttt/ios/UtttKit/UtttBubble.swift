import CUttt
import CoreGraphics
import Messages
import UIKit

/// The transcript bubble: one image, one line of text, both baked at insert.
///
/// THIS IS THE FRAME NOBODY CHOSE. `MSMessageTemplateLayout` renders its image
/// at exactly 300 by 195 points - landscape, aspect 1.54 - and a board is
/// square, so 181 points is the largest one that fits and 119 are left over.
/// The whole design is what those 119 points are for, and the answer is two
/// lines: a headline, and the block the opponent has been sent to.
///
/// AND IT IS BAKED. Every device in the thread shows the one image the sender
/// drew and reads the one caption under it, so neither may say "you" and
/// neither may name a person. There is no name to use anyway: a Messages
/// extension gets a per-conversation UUID for each participant and no way to
/// resolve one to a human. It does not need one - the transcript already says
/// who did it, by which side of the thread the bubble sits on - so the caption
/// is a statement about the board and nothing else.
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

    /// The name of the block the next mark must go in, or "anywhere".
    /// `spoken` is the form a sentence uses: "the bottom-middle board".
    public static func placeName(spoken: Bool) -> String {
        let b = uti_active()
        guard b >= 0, let s = uti_place_name(b, spoken ? 1 : 0) else { return "" }
        return String(cString: s)
    }

    /// Two words. The one thing a glance needs.
    ///
    /// AN EMPTY BOARD IS NOT A MOVE. This said "Your move" at ply zero, which
    /// is wrong for everybody who can see it: the sender has not been dealt a
    /// seat yet and the recipient has not taken one, so there is no move to be
    /// anybody's. The invitation asks the question instead.
    public static var headline: String {
        switch Uttt.over {
        case .draw: return "A draw"
        case .x:    return "X wins"
        case .o:    return "O wins"
        case .none: return Uttt.plyCount == 0 ? "A game?" : "Your move"
        }
    }

    /// The place, in blue, under the headline. A finished game has no place to
    /// send anybody, so it spends the line on how long it took instead.
    public static var place: String {
        if Uttt.over != .none { return "\(Uttt.plyCount) moves" }
        /* AN INVITATION HAS NOWHERE TO SEND ANYBODY. "A game?" is the whole
         * question and a second line under it was a word looking for a job. */
        return Uttt.plyCount == 0 ? "" : placeName(spoken: false)
    }

    /// One line, truncating, and it names who moved. A bubble is the same on
    /// every device, so this is the only sentence that can be written about it.
    /// One line, truncating, and it names NOBODY.
    ///
    /// There is no name to use. A Messages extension is given a
    /// per-conversation UUID for each participant and no way to resolve one
    /// to a person - Apple withholds it - so the only names an app can show
    /// are ones it asked somebody to type, and this game should not have to
    /// ask. It does not need to either: the transcript already says who did
    /// it, by which side of the thread the bubble is on.
    ///
    /// So every caption is a statement about the BOARD. It reads the same to
    /// both players, which is the other half of why it works: one bubble, one
    /// bitmap, one sentence, identical on every device in the thread.
    public static var caption: String {
        switch Uttt.over {
        case .x:    return "X wins."
        case .o:    return "O wins."
        case .draw: return "Nine blocks, no line."
        case .none:
            if Uttt.plyCount == 0 { return newGameCaption }
            if uti_active() == 9 { return "Sent anywhere on the sheet." }
            return "Sent to the \(placeName(spoken: true)) board."
        }
    }

    /// What an unclaimed board says.
    static let newGameCaption = "New Ultimate Tic Tac Toe game"

    /// The moment the roster sealed: the one bubble that is neither a move
    /// nor an invitation.
    public static let sealedCaption = "Both seats taken."

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
        let polys = Uttt.board(active: active, last: last)

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

            /* The kernel draws in a unit square and the board is 181 points in
             * the corner the kernel knows nothing about, so the transform goes
             * on once here rather than into ten thousand multiplications. */
            cg.saveGState()
            cg.translateBy(x: board.minX, y: board.minY)
            cg.scaleBy(x: board.width, y: board.height)
            for poly in polys {
                guard let head = poly.points.first else { continue }
                cg.beginPath()
                cg.move(to: head)
                for p in poly.points.dropFirst() { cg.addLine(to: p) }
                cg.closePath()
                cg.setFillColor(poly.color)
                cg.fillPath()
            }
            cg.restoreGState()

            draw(headline: headline, place: place, in: textBox)
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

    /* EIGHTY-SEVEN POINTS. That is what is left of the 300 once the board has
     * had its 181 and the padding its gutters, and "bottom middle" in bold 16
     * is about 106 - so the headline gets one line and the place is allowed to
     * wrap onto two. Every block name is two short words, so it always breaks
     * cleanly and nothing ever truncates; the truncating line is the caption,
     * which is the one carrying a name it did not choose. */
    private static func draw(headline: String, place: String, in box: CGRect) {
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
        (headline as NSString).draw(
            with: CGRect(x: box.minX, y: y, width: box.width, height: hH),
            options: opts, attributes: hAttr, context: nil)
        y += hH + lead
        (place as NSString).draw(
            with: CGRect(x: box.minX, y: y, width: box.width, height: pH),
            options: opts, attributes: pAttr, context: nil)
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
