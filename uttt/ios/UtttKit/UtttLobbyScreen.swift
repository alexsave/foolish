import SwiftUI

/// Everything that is not the game: the wait, and a bubble this build cannot
/// read.
///
/// THERE IS NO "TAKE A SEAT" SCREEN. The joiner is X and moves first, so
/// opening somebody's invitation opens the board with the first move on it -
/// the join IS the move. And there is no "start" screen either: opening the
/// app through the + menu is the invitation, staged there and then.
///
/// Every word is the kernel's (uttt_say.h).
public struct UtttLobbyScreen: View {

    public enum Stance: Equatable {
        /// You sent the board. Nobody has answered it.
        case waiting
        /// A bubble this build cannot read.
        case unreadable
    }

    public let stance: Stance

    public init(stance: Stance) { self.stance = stance }

    public var body: some View {
        UtttSheet {
            VStack(spacing: 0) {
                Spacer(minLength: 8)
                Text(headline)
                    .font(.system(size: 23, weight: .bold))
                    .foregroundStyle(UtttInk.ink)
                Text(subline)
                    .font(.system(size: 14))
                    .foregroundStyle(UtttInk.muted)
                    .multilineTextAlignment(.center)
                    .padding(.top, 3)
                    .padding(.horizontal, 22)
                Spacer(minLength: 8)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(13)
        }
    }

    /* NO MARK ON THE WAITING SCREEN: the joiner will be X, and until somebody
     * joins there is nobody to be anything. */
    private var headline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingHeadline)
        case .unreadable: return Uttt.say(.unreadableHeadline)
        }
    }

    private var subline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingSubline)
        case .unreadable: return Uttt.say(.unreadableSubline)
        }
    }
}

/// The board, from outside the roster.
///
/// A spectator gets the game and no way to touch it, which is the whole
/// difference - so this is the play surface with the door taken off rather
/// than a screen of its own.
public struct UtttWatchScreen: View {
    @ObservedObject private var model: UtttModel
    public init(model: UtttModel) { self.model = model }

    public var body: some View {
        UtttSheet {
            VStack(spacing: 0) {
                HStack(alignment: .firstTextBaseline) {
                    Text(Uttt.say(.watchLabel))
                        .font(.system(size: 9.5, weight: .semibold))
                        .tracking(1.9)
                        .textCase(.uppercase)
                        .foregroundStyle(UtttInk.label)
                    Spacer()
                    Text(line)
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(UtttInk.ink)
                }
                Spacer(minLength: 6)
                UtttBoard(active: model.active, last: model.last,
                          positionKey: model.positionKey)
                Spacer(minLength: 6)
            }
            .padding(13)
        }
    }

    private var line: String { Uttt.say(.watchLine) }
}

// MARK: - the two pieces both screens share

enum UtttInk {
    static let ink   = Color(red: 0.11, green: 0.106, blue: 0.087)
    static let muted = Color(red: 0.42, green: 0.40,  blue: 0.35)
    static let label = Color(red: 0.54, green: 0.52,  blue: 0.47)
}

/// The one door. Drawn rather than tinted, so it sits on the paper instead of
/// on top of it.
struct UtttDoor: View {
    let title: String
    let act: () -> Void

    var body: some View {
        Button(action: act) {
            Text(title)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(UtttInk.ink)
                .padding(.horizontal, 26)
                .padding(.vertical, 11)
                .background(
                    Capsule().fill(Color.white.opacity(0.55))
                        .overlay(Capsule().strokeBorder(UtttInk.ink.opacity(0.55), lineWidth: 1.4))
                )
        }
        .buttonStyle(.plain)
    }
}
