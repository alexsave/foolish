// COPIED from pickemup/ios/PickemupKit/Board/SeatBadge.swift at 8e216923 (itself COPIED from foolish/ios/FoolishKit/Boards/FSeatBadge.swift at c3d99192) - replaced by the shared-board lift
//
// foolish's badge with the fan of backs replaced by what a dice seat has to
// show: the running total (T10). The 12pt name over it, whose turn it is as a
// brass bar under the total and the name in brass (pickemup's U5), and a tap
// opens that seat's card read-only.
//
// T53: the badges stand in one row across the top of the board, each at most
// pickemup's 96pt name width and never wider than its share of the row.

import SwiftUI

struct SeatBadge: View {
    let seat: Int
    let name: String
    let total: Int
    let isTurn: Bool
    let width: CGFloat
    let onTap: () -> Void

    static let nameMax: CGFloat = 96
    static let height: CGFloat = 44

    var body: some View {
        VStack(spacing: 1) {
            Text(name)
                .font(.system(size: 12, weight: .semibold))
                .onFeltText(isTurn ? FColor.win : FColor.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
                .minimumScaleFactor(0.7)
            Text("\(total)")
                .font(.system(size: 17, weight: .heavy).monospacedDigit())
                .onFeltText(isTurn ? FColor.win : FColor.textPrimary)
                .overlay(alignment: .bottom) {
                    if isTurn {
                        RoundedRectangle(cornerRadius: 2).fill(FColor.win)
                            .frame(width: 28, height: 3).offset(y: 5)
                            .shadow(color: FColor.win.opacity(0.7), radius: 4)
                    }
                }
        }
        .frame(width: width, height: Self.height)
        .contentShape(Rectangle())
        .onTapGesture { onTap() }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .tbAnchor("seat.\(seat)")
    }
}
