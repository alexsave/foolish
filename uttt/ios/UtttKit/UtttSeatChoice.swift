#if DEBUG
import SwiftUI

/// WHICH OF THE TWO PEOPLE YOU ARE, asked once when a bubble is opened.
///
/// A two-handed game in a transcript cannot be played on one phone: Messages
/// gives a conversation exactly one local participant, so the invitation goes
/// out and nothing ever happens. With `dev.picker` set, opening a bubble asks
/// this first and then gets out of the way - the game plays normally
/// afterwards, with no debug anything on any screen.
///
/// IT DOES NOT NAME THE MARKS. Which seat is X is not decided until both are
/// taken, and a chooser that leaked it would be a nicer version of the hole
/// this app was just fixed to close.
public struct UtttSeatChoice: View {
    private let pick: (String) -> Void
    public init(pick: @escaping (String) -> Void) { self.pick = pick }

    private static let ink   = Color(red: 0.114, green: 0.106, blue: 0.086)
    private static let label = Color(red: 0.541, green: 0.522, blue: 0.467)

    public var body: some View {
        UtttSheet { sheet }
    }

    /// ON THE NAPKIN like every other screen. Without the sheet this came up
    /// as dark ink on the drawer's own dark background and read as an empty
    /// box - which is what a screen looks like when it has no paper.
    private var sheet: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 8)
            Text("Who are you?")
                .font(.system(size: 21, weight: .bold))
                .tracking(-0.315)
                .foregroundStyle(Self.ink)
            Text("One phone, two people. Pick a side and play.")
                .font(.system(size: 13))
                .foregroundStyle(Self.label)
                .padding(.top, 5)
            HStack(spacing: 12) {
                seat("a", "The one who\nput it down")
                seat("b", "The one who\ntook it up")
            }
            .padding(.top, 20)
            Spacer(minLength: 8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(13)
    }

    private func seat(_ word: String, _ title: String) -> some View {
        Button { pick(word) } label: {
            Text(title)
                .font(.system(size: 14, weight: .semibold))
                .multilineTextAlignment(.center)
                .foregroundStyle(Self.ink)
                .frame(width: 132, height: 66)
                .background(
                    RoundedRectangle(cornerRadius: 33)
                        .stroke(Self.label.opacity(0.7), lineWidth: 1.2)
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
#endif
