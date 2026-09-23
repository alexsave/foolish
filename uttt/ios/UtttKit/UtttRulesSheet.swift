import SwiftUI

/// What the door opens on.
///
/// THE TEXT IS THE KERNEL'S. Six lines and a title come out of `uttt_draw.c`
/// for the same reason the nine block names do - it is the one thing that
/// knows what the rules are, and a second copy of them in a renderer is a
/// second rulebook that drifts. This file lays them out and nothing else.
///
/// A SHEET OF ITS OWN, on the same paper (`rulebook` below). It used to
/// replace the board inside the drawer, so a swipe down on the rules was a
/// swipe on Messages' drawer and collapsed or closed the game (owner,
/// 2026-09-23). Presented as its own sheet it carries its own drag to
/// dismiss, and the swipe closes the RULES - foolish presents its rulebook
/// the same way (GameSurface `.sheet(isPresented: $showRules)`).
public struct UtttRulesSheet: View {
    private let close: () -> Void
    public init(close: @escaping () -> Void) { self.close = close }

    /// The margin round the rules, the same on every edge.
    static let margin: CGFloat = 13

    private static let ink   = Color(red: 0.114, green: 0.106, blue: 0.086)
    private static let label = Color(red: 0.541, green: 0.522, blue: 0.467)
    private static let blue  = Color(red: 0.145, green: 0.216, blue: 0.420)

    public var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .firstTextBaseline) {
                Text(Uttt.rulesTitle)
                    .font(.system(size: 21, weight: .bold))
                    .tracking(-0.315)
                    .foregroundStyle(Self.ink)
                Spacer(minLength: 12)
                Button(action: close) {
                    Text("Back")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Self.blue)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            .padding(.bottom, 14)

            ScrollView(.vertical, showsIndicators: false) {
                VStack(alignment: .leading, spacing: 13) {
                    ForEach(Array(Uttt.rules.enumerated()), id: \.offset) { i, line in
                        HStack(alignment: .firstTextBaseline, spacing: 9) {
                            /* The count is a drawn thing too - a tally, not a
                             * numeral, because a digit set in a font would be
                             * the only printed character on the sheet. */
                            Text(String(repeating: "|", count: i + 1))
                                .font(.system(size: 11, weight: .bold))
                                .tracking(1.2)
                                .foregroundStyle(Self.label)
                                .frame(width: 44, alignment: .leading)
                            Text(line)
                                .font(.system(size: 15))
                                .foregroundStyle(Self.ink)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                .padding(.bottom, 8)
            }
        }
    }
}

extension View {
    /// The rulebook over this screen while `open` is true: its own sheet on
    /// the napkin's paper, dismissed by its own swipe down or by Back.
    public func rulebook(_ open: Binding<Bool>) -> some View {
        sheet(isPresented: open) {
            UtttSheet {
                UtttRulesSheet { open.wrappedValue = false }
                    .padding(UtttRulesSheet.margin)
            }
            .presentationDragIndicator(.visible)
        }
    }
}
