import SwiftUI

/// Everything that is not the game: the wait, an invitation that was taken
/// back, and a bubble this build cannot read.
///
/// THERE IS NO "TAKE A SEAT" SCREEN. The joiner is X and moves first, so
/// opening somebody's invitation opens the board with the first move on it -
/// the join IS the move. And there is no "start" screen either: opening the
/// app through the + menu is the invitation, staged there and then.
///
/// docs/UI.html, "Lobby and end", 02: a headline, a line under it, the empty
/// board, and one door at the bottom. Which door, and whether there is one at
/// all, is the kernel's (utm_door); every word is the kernel's (uttt_say.h).
public struct UtttLobbyScreen: View {

    public enum Stance: Equatable {
        /// You sent the board. Nobody has answered it.
        case waiting
        /// Its creator took it back. Nobody can sit down at it.
        case closed
        /// A bubble this build cannot read.
        case unreadable
    }

    public let stance: Stance
    public let door: Uttt.Door
    private let onDoor: () -> Void

    public init(stance: Stance, door: Uttt.Door = .none, onDoor: @escaping () -> Void = {}) {
        self.stance = stance
        self.door = door
        self.onDoor = onDoor
    }

    public var body: some View {
        UtttSheet {
            GeometryReader { geo in
                if geo.size.height > UtttDoorButton.expandedFrom {
                    expanded
                } else {
                    compact
                }
            }
        }
    }

    /// docs/UI.html 02 as drawn: the words, the board under them, the door.
    private var expanded: some View {
        VStack(alignment: .leading, spacing: 0) {
            words
            if stance != .unreadable {
                /* ROOM FOR THE OVERSHOOT: the grid's main lines run past the
                 * board by about a tenth of it, and at 10 points they ran up
                 * into the line of type above. */
                board.padding(.vertical, 26)
            } else {
                Spacer(minLength: 0)
            }
            doorView.padding(.top, 10)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(13)
    }

    /// THE COMPACT STRIP PUTS THE WORDS BESIDE THE BOARD, the way the play
    /// surface puts "you are" beside it: stacked, the three things left a
    /// 112-point board in 309 points of drawer.
    private var compact: some View {
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 0) {
                words
                Spacer(minLength: 8)
                doorView
            }
            .frame(width: 168, alignment: .leading)
            if stance != .unreadable {
                board.padding(.vertical, 14)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(.horizontal, 13)
        .padding(.vertical, 10)
    }

    private var words: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(headline)
                .font(.system(size: 21, weight: .bold))
                .tracking(-0.315)
                .foregroundStyle(UtttInk.ink)
                .lineLimit(1)
            Text(subline)
                .font(.system(size: 14))
                .foregroundStyle(UtttInk.muted)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /* THE EMPTY BOARD, with no wash: nobody is on move, and a board tinted
     * corner to corner reads as a different piece of paper. */
    private var board: some View {
        UtttBoard(active: -1, last: -1, positionKey: 0)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
    }

    @ViewBuilder private var doorView: some View {
        if let title = UtttDoorButton.title(door) {
            UtttDoorButton(title: title, ghost: door == .takeBack, act: onDoor)
        }
    }

    /* NO MARK ON THE WAITING SCREEN: the joiner will be X, and until somebody
     * joins there is nobody to be anything. */
    private var headline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingHeadline)
        case .closed:     return Uttt.say(.closedHeadline)
        case .unreadable: return Uttt.say(.unreadableHeadline)
        }
    }

    private var subline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingSubline)
        case .closed:     return Uttt.say(.closedSubline)
        case .unreadable: return Uttt.say(.unreadableSubline)
        }
    }
}

/// The board, from outside the roster.
///
/// A spectator gets the game and no way to touch it, which is the whole
/// difference - so this is the play surface with the door taken off rather
/// than a screen of its own. At the end it does get the one door everybody
/// gets: Again.
public struct UtttWatchScreen: View {
    @ObservedObject private var model: UtttModel
    private let door: Uttt.Door
    private let onDoor: () -> Void

    public init(model: UtttModel, door: Uttt.Door = .none, onDoor: @escaping () -> Void = {}) {
        self.model = model
        self.door = door
        self.onDoor = onDoor
    }

    public var body: some View {
        UtttSheet {
            GeometryReader { geo in
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
                    /* Again belongs to the expanded view (UI.html 08). */
                    if geo.size.height > UtttDoorButton.expandedFrom,
                       let title = UtttDoorButton.title(door) {
                        UtttDoorButton(title: title, ghost: false, act: onDoor)
                    }
                }
                .padding(13)
            }
        }
    }

    private var line: String { Uttt.say(.watchLine) }
}

// MARK: - the pieces the screens share

enum UtttInk {
    static let ink   = Color(red: 0.11, green: 0.106, blue: 0.087)
    static let muted = Color(red: 0.42, green: 0.40,  blue: 0.35)
    static let label = Color(red: 0.54, green: 0.52,  blue: 0.47)
    static let blue  = Color(red: 0.145, green: 0.216, blue: 0.420) // #25376b
}

/// THE ONE DOOR, as docs/UI.html draws it (`.udoor`): a full-width slab at the
/// bottom of the sheet, blue with pale type for a door that starts something
/// (Again), and a ghost - a faint fill and a hairline - for the one that takes
/// something back.
public struct UtttDoorButton: View {
    let title: String
    let ghost: Bool
    let act: () -> Void

    /// ABOVE THIS HEIGHT THE DRAWER IS EXPANDED. Messages hands the compact
    /// drawer 340 points at the most (323 with the keyboard up) and the
    /// expanded one 541 at the least (an SE), so anything between is a drawer
    /// in motion, and 440 splits it.
    public static let expandedFrom: CGFloat = 440

    /// The words on a door, or nil for no door. The kernel's.
    public static func title(_ door: Uttt.Door) -> String? {
        switch door {
        case .none:     return nil
        case .takeBack: return Uttt.say(.doorTakeBack)
        case .again:    return Uttt.say(.doorAgain)
        }
    }

    private static let grey = Color(red: 40 / 255, green: 38 / 255, blue: 32 / 255)

    public var body: some View {
        Button(action: act) {
            Text(title)
                .font(.system(size: 14, weight: .bold))
                .tracking(0.56)                         // .04em
                .foregroundStyle(ghost ? UtttInk.muted
                                       : Color(red: 0.957, green: 0.965, blue: 0.984))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 13)
                .background(
                    RoundedRectangle(cornerRadius: 9)
                        .fill(ghost ? Self.grey.opacity(0.07) : UtttInk.blue)
                        .shadow(color: .black.opacity(ghost ? 0 : 0.28), radius: 0, x: 0, y: 2)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 9)
                        .strokeBorder(Self.grey.opacity(ghost ? 0.22 : 0), lineWidth: 1)
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
