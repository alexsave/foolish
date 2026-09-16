// FSeatTag.swift - a seat on the PUBLIC board: the name over ONE landscape
// card back carrying the hand count, with the role mark right beside it.
//
// The live board's FSeatBadge fans a card back per card in hand and hangs a
// 40pt role row under the fan, 103pt tall in all. That is the right badge for
// a board you play on; on the 300x195 bubble it is the reason eight seats did
// not fit (PublicBoardLayout). The owner's two suggestions for the bubble -
// "replace the fan with a single horizontal card carrying a number" and
// "bring the status icon closer to the card" - are both this view: the count
// IS the information the fan was carrying (FSeatBadge already learned that a
// count and a picture must agree, and put the truth in the number), and the
// mark sits 2pt from the card instead of a row below it.
//
// What it carries is exactly what FSeatBadge carries - name, count, one mark,
// out - so nothing is lost in the shrink; it only stops drawing the same fact
// twice. Same ink rules as the badge (FSeatBadge.nameInk / nameShadow), the
// same count chip, the same three marks from the same drawings
// (RoleMarkView), so the tag reads as the badge seen from further away.

import SwiftUI

struct FSeatTag: View {
    @Environment(\.colorScheme) private var scheme
    let name: String
    let handCount: Int
    let mark: RoleMarkKind?
    let isOut: Bool

    init(name: String, handCount: Int, mark: RoleMarkKind?, isOut: Bool = false) {
        self.name = name
        self.handCount = handCount
        self.mark = mark
        self.isOut = isOut
    }

    /// The whole tag, and the box PublicBoardLayout reserves for it.
    static let size = PublicBoardLayout.tagSize
    /// The one card back, portrait before it is laid on its side: 25x36 is
    /// the deck well's 46x66 at just over half, and lands the count chip's
    /// 15pt digits - the size the live board's badge draws them at - on a
    /// 36pt-wide face with air either side of "12". (24x34 with 13pt digits
    /// was the first cut; the owner asked for bigger counts and badges, and
    /// this is as big as three tags across the top of an eight-seat bubble
    /// leave room for beside the corner pieces - PublicBoardLayout.rowSpread.)
    static let cardSize = CGSize(width: 25, height: 36)
    static let countSize: CGFloat = 15
    static let countFont = Font.system(size: countSize, weight: .bold)
    /// The marks at 0.6 of FRoleMark - sword 24, shield 20, check 16 - so the
    /// row stays one line tall and, with the card and 2pt, exactly
    /// `PublicBoardLayout.tagSize.width`.
    static let markScale: CGFloat = 0.6
    static let markBox: CGFloat = FRoleMark.rowHeight * markScale

    var body: some View {
        VStack(spacing: 1) {
            Text(name)
                .font(FType.body(12))
                .fontWeight(.semibold)
                .foregroundColor(ink)
                .shadow(color: shadow, radius: shadow == .clear ? 0 : 2,
                        y: shadow == .clear ? 0 : 1)
                .lineLimit(1)
                .truncationMode(.tail)
                .minimumScaleFactor(0.7)
                .frame(maxWidth: Self.size.width)
            HStack(spacing: 2) {
                ZStack {
                    // An out seat shows its name and nothing else, like the
                    // badge's empty fan (web parity).
                    if handCount > 0 {
                        FCard(card: nil, backSeed: 7, size: Self.cardSize)
                            .rotationEffect(.degrees(90))
                        FCountChip("\(handCount)", font: Self.countFont)
                    }
                }
                // The rotated card's visual box; `rotationEffect` leaves the
                // layout size portrait (FDeckWell's rotationNudge is the same
                // fact, handled the same way).
                .frame(width: Self.cardSize.height, height: Self.cardSize.width)
                ZStack {
                    if let mark { RoleMarkView(mark, scale: Self.markScale) }
                }
                // A constant box whether or not the seat wears a mark, so the
                // card sits in the same place on every tag (FRoleCoin does the
                // same for the badge).
                .frame(width: Self.markBox, height: Self.markBox)
            }
        }
        .frame(width: Self.size.width, height: Self.size.height)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(a11y)
    }

    /// The tag always sits on the wool (never the beige bubble the badge's
    /// `onLight` was for), so the ink and shadow are the badge's wool case.
    private var ink: Color { FSeatBadge.nameInk(isOut: isOut, onLight: false, scheme: scheme) }
    private var shadow: Color { FSeatBadge.nameShadow(isOut: isOut, onLight: false, scheme: scheme) }

    private var a11y: String {
        var parts = ["\(name), \(FStrings.t("ios.a11y.cards", ["n": "\(handCount)"]))"]
        switch mark {
        case .shield?: parts.append(FStrings.t("ios.a11y.defending"))
        case .sword?, .leadSword?: parts.append(FStrings.t("ios.a11y.attacking"))
        case .check?: parts.append(FStrings.t("ios.a11y.saidgood"))
        case nil: break
        }
        if isOut { parts.append(FStrings.t("ios.a11y.out")) }
        return parts.joined(separator: ", ")
    }
}
