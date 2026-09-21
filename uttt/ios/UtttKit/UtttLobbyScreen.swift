import SwiftUI

/// Everything that is not the game: the invitation, the wait, the seat that is
/// still open, and the view from outside the roster.
///
/// THE ROSTER SEALS AT TWO. A third person in a group chat is not a problem to
/// be handled, it is a spectator - so the shape of this file is four stances
/// and one door, and only two of the stances have a door at all.
public struct UtttLobbyScreen: View {

    public enum Stance: Equatable {
        /// Nothing in this thread yet. The door sends the empty board, and the
        /// moment it does is the seed.
        case start
        /// You sent the board. Nobody has answered it.
        case waiting(Uttt.Mark?)
        /// The second seat is open and it would be yours.
        case open(Uttt.Mark?)
        /// A bubble this build cannot read.
        case unreadable
    }

    public let stance: Stance
    /// Only used to draw the mark; there is no seed before `.start` sends one.
    public let seed: Int32
    public let act: () -> Void

    public init(stance: Stance, seed: Int32 = 1, act: @escaping () -> Void) {
        self.stance = stance
        self.seed = seed
        self.act = act
    }

    public var body: some View {
        UtttSheet {
            VStack(spacing: 0) {
                Spacer(minLength: 8)
                if let m = mark {
                    VStack(spacing: 4) {
                        Text("you\nare")
                            .font(.system(size: 9.5, weight: .semibold))
                            .tracking(1.9)
                            .textCase(.uppercase)
                            .multilineTextAlignment(.center)
                            .foregroundStyle(UtttInk.label)
                        UtttMarkIcon(mark: m, seed: seed &+ 4)
                            .frame(width: 54, height: 54)
                    }
                    .padding(.bottom, 14)
                }
                Text(headline)
                    .font(.system(size: 23, weight: .bold))
                    .foregroundStyle(UtttInk.ink)
                Text(subline)
                    .font(.system(size: 14))
                    .foregroundStyle(UtttInk.muted)
                    .multilineTextAlignment(.center)
                    .padding(.top, 3)
                    .padding(.horizontal, 22)
                if let door {
                    UtttDoor(title: door, act: act).padding(.top, 18)
                }
                Spacer(minLength: 8)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(13)
        }
    }

    private var mark: Uttt.Mark? {
        switch stance {
        case .waiting(let m), .open(let m): return m
        case .start, .unreadable:           return nil
        }
    }

    private var headline: String {
        switch stance {
        case .start:      return "Ultimate tic-tac-toe"
        case .waiting:    return "Waiting"
        case .open:       return "There is a seat"
        case .unreadable: return "Can't read that"
        }
    }

    private var subline: String {
        switch stance {
        case .start:
            /* NOT "Send an empty board" - the button under this line says
             * that, and a subtitle that narrates the button is a line nobody
             * reads twice. This says the one thing the button cannot. */
            return "Whoever answers it first takes the other side."
        case .waiting:
            /* NO MARK, and no hint of one. Which seat is whose is not decided
             * until both are filled, so there is nothing here to re-roll for
             * - which is the whole security property. */
            return "Nobody has taken the other side yet."
        case .open:
            return "Take it and the sides are drawn. Neither of you picks."
        case .unreadable:
            return "That board came from a newer version of the app."
        }
    }

    private var door: String? {
        switch stance {
        case .start:                 return "Send a board"
        case .open:                  return "Take it"
        case .waiting, .unreadable:  return nil
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
                    Text("watching")
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

    private var line: String {
        switch Uttt.over {
        case .draw: return "Drawn"
        case .x:    return "X took it"
        case .o:    return "O took it"
        case .none: return Uttt.turn == .o ? "O to play" : "X to play"
        }
    }
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
