import SwiftUI

/// What the door opens on.
///
/// THE TEXT IS THE KERNEL'S. Six lines and a title come out of `uttt_draw.c`
/// for the same reason the nine block names do - it is the one thing that
/// knows what the rules are, and a second copy of them in a renderer is a
/// second rulebook that drifts. This file lays them out and nothing else.
///
/// ON THE SAME PIECE OF PAPER, and deliberately not a modal card floating
/// over a dimmed board. The sheet in a Messages drawer is already a piece of
/// paper in a small box; putting a second surface on top of it would be the
/// only thing in the app that is not drawn on the napkin.
public struct UtttRulesSheet: View {
    private let close: () -> Void
    public init(close: @escaping () -> Void) { self.close = close }

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
