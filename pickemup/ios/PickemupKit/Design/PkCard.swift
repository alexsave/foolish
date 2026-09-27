// COPIED from foolish/ios/FoolishKit/DesignSystem/FCard.swift at c3d99192 - replaced by lift step S10
//
// foolish's card FRAME, unchanged in what it is: the rounded rect with radius
// min(5, 0.1w), the cream face (#F4EFE6, U1), the locked deep-red #8B1A1A
// 1pt edge on face and back alike, the baked fern back, the 4pt #FF2A22
// selection ring drawn with strokeBorder so it moves nothing, and the thin
// face under 40pt (U8). No shadow, no glow.
//
// THE CONTENT IS THIS PRODUCT'S (REUSE_AUDIT.md 4: "the frame is neutral and
// the face is not"), drawn as UI.html draws it: a shape AND a colour per suit
// (circle teal, triangle amber, square violet, diamond slate), numbers 1-9,
// Skip as two pause bars, Reverse as two opposed solid triangles on bars, +2,
// Wild as the four suits together, Wild +4 as that with "+4". Never the
// oval, never the trademark's word (pickemup/LEGAL.md). What a card id IS
// comes from the kernel (Pk.cardSuit / Pk.cardRank); the id order is never
// decoded here.

import CPickemup
import SwiftUI

/// The four suits' inks and the wild's, as the study's CSS names them
/// (--s-circle, --s-tri, --s-square, --s-diamond, --s-wild).
public enum SuitInk {
    public static func color(_ suit: Int) -> Color {
        switch suit {
        case 0:  return Color(hex: 0x15706C)
        case 1:  return Color(hex: 0xBD8014)
        case 2:  return Color(hex: 0x63459A)
        case 3:  return Color(hex: 0x2F4858)
        default: return Color(hex: 0x3A3A42)
        }
    }
    /// The halo behind the pile top: the live suit at half strength.
    public static func halo(_ suit: Int) -> Color { color(suit).opacity(0.5) }
}

/// What a card id is, by the kernel's word.
public struct CardFace: Equatable {
    public let id: Int
    public let suit: Int          // 0..3, PK_NO_SUIT for a wild
    public let rank: Int          // 1..9 or PK_R_*
    public init?(_ id: Int) {
        let s = Int(pk_api_card_suit(Int32(id))), r = Int(pk_api_card_rank(Int32(id)))
        guard s >= 0, r > 0 else { return nil }
        self.id = id; suit = s; rank = r
    }
    public var isWild: Bool { rank == PK_R_WILD || rank == PK_R_WILD4 }
    public var isNumber: Bool { rank >= 1 && rank <= 9 }
    /// O6: Skip, Reverse and +2 carry their suit's SHAPE small in the corners,
    /// so an action card is readable without its colour. nil on number cards
    /// (the big centre glyph is the shape) and on wilds (no suit).
    public var cornerSuit: Int? {
        rank == PK_R_SKIP || rank == PK_R_REVERSE || rank == PK_R_PLUS2 ? suit : nil
    }
    /// The index in the corner and on the pip, if the face prints one: the
    /// kernel's word (PK_API_W_INDEX), "" for a face that prints a glyph.
    var label: String? {
        let w = Pk.words(PK_API_W_INDEX, id)
        return w.isEmpty ? nil : w
    }
}

// MARK: the glyphs, in UI.html's 100-unit box

/// One of the study's glyphs, drawn in a square frame.
public enum Glyph: Sendable, Equatable { case suit(Int), skip, reverse }

struct GlyphShape: Shape {
    let glyph: Glyph

    func path(in r: CGRect) -> Path {
        let s = min(r.width, r.height) / 100
        let ox = r.midX - 50 * s, oy = r.midY - 50 * s
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: ox + x * s, y: oy + y * s) }
        func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ rr: CGFloat) -> Path {
            Path(roundedRect: CGRect(x: ox + x * s, y: oy + y * s, width: w * s, height: h * s),
                 cornerRadius: rr * s)
        }
        func poly(_ pts: [(CGFloat, CGFloat)]) -> Path {
            var path = Path()
            path.move(to: p(pts[0].0, pts[0].1))
            for q in pts.dropFirst() { path.addLine(to: p(q.0, q.1)) }
            path.closeSubpath()
            return path
        }
        switch glyph {
        case .suit(0):
            return Path(ellipseIn: CGRect(x: ox + 16 * s, y: oy + 16 * s, width: 68 * s, height: 68 * s))
        case .suit(1):
            return poly([(50, 14), (86, 76), (14, 76)])
        case .suit(2):
            return rect(18, 18, 64, 64, 7)
        case .suit(3):
            return poly([(50, 11), (87, 50), (50, 89), (13, 50)])
        case .suit:
            return Path()
        case .skip:
            var path = rect(26, 20, 17, 60, 4)
            path.addPath(rect(57, 20, 17, 60, 4))
            return path
        case .reverse:
            var path = poly([(62, 14), (92, 34), (62, 54)])
            path.addPath(rect(10, 27, 58, 14, 5))
            path.addPath(poly([(38, 46), (8, 66), (38, 86)]))
            path.addPath(rect(32, 59, 58, 14, 5))
            return path
        }
    }
}

/// The wild's face: the four suits together, each in its own ink.
struct WildGlyph: View {
    var body: some View {
        GeometryReader { geo in
            let s = min(geo.size.width, geo.size.height) / 100
            ZStack(alignment: .topLeading) {
                Circle().fill(SuitInk.color(0))
                    .frame(width: 30 * s, height: 30 * s).offset(x: 35 * s, y: 11 * s)
                Path { p in
                    p.move(to: CGPoint(x: 74 * s, y: 36 * s)); p.addLine(to: CGPoint(x: 90 * s, y: 64 * s))
                    p.addLine(to: CGPoint(x: 58 * s, y: 64 * s)); p.closeSubpath()
                }.fill(SuitInk.color(1))
                RoundedRectangle(cornerRadius: 4 * s).fill(SuitInk.color(2))
                    .frame(width: 26 * s, height: 26 * s).offset(x: 37 * s, y: 61 * s)
                Path { p in
                    p.move(to: CGPoint(x: 26 * s, y: 34 * s)); p.addLine(to: CGPoint(x: 42 * s, y: 50 * s))
                    p.addLine(to: CGPoint(x: 26 * s, y: 66 * s)); p.addLine(to: CGPoint(x: 10 * s, y: 50 * s))
                    p.closeSubpath()
                }.fill(SuitInk.color(3))
            }
            .frame(width: 100 * s, height: 100 * s)
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

/// A suit's shape in its ink, as the picker tiles and the strip chips draw it.
public struct SuitMark: View {
    let suit: Int
    var ink: Color? = nil
    public init(suit: Int, ink: Color? = nil) { self.suit = suit; self.ink = ink }
    public var body: some View {
        GlyphShape(glyph: .suit(suit)).fill(ink ?? SuitInk.color(suit))
    }
}

// MARK: the card

public struct PkCard: View {
    /// A card id, or nil for a back (a hidden card is a back too).
    public let card: Int?
    public var size: CGSize
    public var selected = false
    public var dimmed = false
    /// Draw the full face however narrow (an overlapped hand card, U8).
    public var fullFace = false
    /// A played wild's chosen suit: the band along its foot (U15).
    public var chosen: Int? = nil
    /// The motion name the band slides up under (the pile's top card only):
    /// PkFX.band, the kernel's BAND beat (ANIMATION_DECISIONS A12).
    public var bandFX: String? = nil
    /// The staged strip's 12 x 17 chip: the glyph alone, no index and no pip
    /// (UI.html `.strip .chip .cf`).
    public var chip = false

    public init(card: Int?, size: CGSize, selected: Bool = false, dimmed: Bool = false,
                fullFace: Bool = false, chosen: Int? = nil, chip: Bool = false) {
        self.card = card; self.size = size; self.selected = selected
        self.dimmed = dimmed; self.fullFace = fullFace; self.chosen = chosen; self.chip = chip
    }

    /// This card's band slides up under the motion `name` (the pile's top).
    public func slidingBand(_ name: String) -> PkCard {
        var c = self
        c.bandFX = name
        return c
    }

    private var radius: CGFloat { chip ? 2 : min(5, size.width * 0.1) }
    private var thin: Bool { !fullFace && size.width < PkLayout.thinBelow }
    private static let selWidth: CGFloat = 4
    private static let restWidth: CGFloat = 1

    public var body: some View {
        Group {
            if let card, let face = CardFace(card) { faceView(face) } else { back }
        }
        .frame(width: size.width, height: size.height)
        .opacity(dimmed ? 0.5 : 1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(spokenCard)
        .accessibilityValue(spokenCorner)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    /// The card by the kernel's word ("skip on squares"); empty for a back.
    private var spokenCard: String {
        guard let card, CardFace(card) != nil else { return "" }
        return Pk.words(PK_API_W_CARD, card)
    }

    /// O6: the shape printed in an action card's corners, by the kernel's
    /// one-shape noun ("square"), so what the corners draw is also what is read.
    private var spokenCorner: String {
        guard let card, let s = CardFace(card)?.cornerSuit else { return "" }
        return Pk.string("SUIT_ONE_\(s)")
    }

    private var edge: some View {
        RoundedRectangle(cornerRadius: radius)
            .strokeBorder(selected ? FColor.selRed : FColor.deepRed,
                          lineWidth: selected ? Self.selWidth : Self.restWidth)
    }

    private var back: some View {
        RoundedRectangle(cornerRadius: radius)
            .fill(Color.black)
            .overlay {
                if let img = PkTextures.fernBack {
                    Image(uiImage: img).resizable().scaledToFill()
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: radius))
            .overlay(edge)
    }

    @ViewBuilder private func faceView(_ f: CardFace) -> some View {
        let w = size.width, h = size.height
        let ink = SuitInk.color(f.suit)
        let rankSize = w * (thin ? 0.56 : 0.36)
        // the glyph's box as a fraction of the card: x, y, w, h (UI.html .g,
        // .cf.thin .g, .cf.thin.wildc .g, .strip .chip .cf .g)
        let g: (CGFloat, CGFloat, CGFloat, CGFloat) =
            chip ? (0.12, 0.18, 0.76, 0.64)
            : thin ? (0.22, f.isWild ? 0.34 : 0.52, 0.56, f.isWild ? 0.50 : 0.36)
            : (0.19, 0.21, 0.62, 0.62)
        RoundedRectangle(cornerRadius: radius)
            .fill(FColor.card)
            .overlay(alignment: .topLeading) {
                glyph(f, ink)
                    .frame(width: w * g.2, height: h * g.3)
                    .offset(x: w * g.0, y: h * g.1)
            }
            .overlay {
                if !thin, !chip, let label = f.label {
                    Text(label)
                        .font(.custom("Georgia", size: w * 0.44 * (f.isNumber ? 1 : 0.7)).weight(.bold))
                        .foregroundColor(Color(hex: 0xFBF8F1))
                        .shadow(color: .black.opacity(f.isWild ? 0.7 : 0.35), radius: f.isWild ? 1.5 : 1, y: 1)
                        .fixedSize()
                        .offset(y: h * 0.02)
                }
            }
            .overlay(alignment: thin ? .top : .topLeading) { if !chip { corner(f, rankSize) } }
            .overlay(alignment: .bottomTrailing) {
                // O6: the action card's second corner, the first turned half round
                if !chip, !thin, f.cornerSuit != nil { corner(f, rankSize).rotationEffect(.degrees(180)) }
            }
            .overlay(alignment: .bottom) {
                if let chosen, f.isWild {
                    BandSlide(name: bandFX, height: h * 0.16) {
                        Rectangle().fill(SuitInk.color(chosen))
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: radius))
            .overlay(edge)
    }

    @ViewBuilder private func glyph(_ f: CardFace, _ ink: Color) -> some View {
        if f.isWild {
            WildGlyph()
        } else if f.rank == PK_R_SKIP {
            GlyphShape(glyph: .skip).fill(ink)
        } else if f.rank == PK_R_REVERSE {
            GlyphShape(glyph: .reverse).fill(ink)
        } else {
            GlyphShape(glyph: .suit(f.suit)).fill(ink)
        }
    }

    /// The corner index, UI.html's `.cr` column: the rank (or +2 / +4), or
    /// the action's own glyph for Skip and Reverse; nothing on a plain wild.
    /// An action card's column carries its suit's shape under the index (O6),
    /// thin faces included, since a hand's overlapped cards show only this.
    /// Thin: centred, and the Skip / Reverse glyph is the body's, not the corner's.
    @ViewBuilder private func corner(_ f: CardFace, _ rankSize: CGFloat) -> some View {
        let ink = SuitInk.color(f.suit)
        VStack(spacing: 1) {
            if let label = f.label {
                Text(label).font(.custom("Georgia", size: rankSize).weight(.bold)).foregroundColor(ink).fixedSize()
            } else if !thin, f.rank == PK_R_SKIP || f.rank == PK_R_REVERSE {
                GlyphShape(glyph: f.rank == PK_R_SKIP ? .skip : .reverse).fill(ink)
                    .frame(width: rankSize * 0.72, height: rankSize * 0.72)
            }
            if let s = f.cornerSuit {
                SuitMark(suit: s).frame(width: rankSize * 0.5, height: rankSize * 0.5)
            }
        }
        .padding(.leading, thin ? 0 : size.width * 0.08)
        .padding(.top, size.height * (thin ? 0.14 : 0.04))
    }
}

/// The chosen-suit band, slid up from under the card's foot as far as the
/// kernel's BAND beat has taken it (UI.html DEMO.wild: translateY(100%) to 0,
/// clipped by the card). With no name, or no beat playing, it is in place.
struct BandSlide<Content: View>: View {
    let name: String?
    let height: CGFloat
    @ViewBuilder let content: () -> Content
    @Environment(\.pkFX) private var all

    var body: some View {
        let up = name.flatMap { all[$0]?.band } ?? 1
        content()
            .frame(height: height)
            .offset(y: (1 - up) * height)
    }
}
