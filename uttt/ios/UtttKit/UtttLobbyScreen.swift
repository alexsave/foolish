import SwiftUI

/// Everything that is not the game: the wait, and a bubble this build cannot
/// read.
///
/// THERE IS NO "TAKE A SEAT" SCREEN. The joiner is X and moves first, so
/// opening somebody's invitation opens the board with the first move on it -
/// the join IS the move. And there is no "start" screen either: opening the
/// app through the + menu is the invitation, staged there and then.
///
/// docs/UI.html, "Lobby and end", 02: a headline, a line under it and the
/// empty board. WITHOUT the spec's "Take it back" door: the owner decided
/// (2026-09-22) there is no undo and no take-back - the draft's own X is the
/// only way back. Every word is the kernel's (uttt_say.h).
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
            GeometryReader { geo in
                if geo.size.height > UtttDoorButton.expandedFrom {
                    expanded
                } else {
                    compact
                }
            }
        }
    }

    /// docs/UI.html 02 as drawn: the words and the board under them.
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
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(13)
    }

    /// THE COMPACT STRIP PUTS THE WORDS BESIDE THE BOARD, the way the play
    /// surface puts "you are" beside it: stacked, they left a 112-point board
    /// in 309 points of drawer.
    private var compact: some View {
        HStack(alignment: .top, spacing: 12) {
            /* THE WORDS TAKE THEIR OWN WIDTH. A fixed 150 broke "Nobody has
             * taken it yet." after "taken", leaving "it yet." alone on a
             * second line; UI.html 02 sets it as one line under the headline,
             * and the board gives way instead. */
            words.fixedSize(horizontal: true, vertical: false)
            /* The overshoot needs room on every side, or it runs into the
             * drawer's edge and the grab handle. */
            if stance != .unreadable {
                board.padding(22)
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
                        UtttDoorButton(title: title, act: onDoor)
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
    /// The doors' outline ink (uttt_rule.c EDGE, #1b2a52), which the label
    /// is set in so the word and the bar are one pen.
    static let doorInk = Color(red: 0.106, green: 0.165, blue: 0.322)
}

/// THE ONE DOOR: docs/UI.html's `.udoor go` (full width, bottom of the
/// sheet, 14-point bold type at .04em), drawn by the pen rather than printed.
/// The bar - a rough outline over a two-fifths hachure, the rulebook square's
/// pen - is `uttt_draw_door`; this view fills its polygons at the size it
/// has and sets the label on top in the outline's dark ink.
public struct UtttDoorButton: View {
    let title: String
    let height: CGFloat
    let act: () -> Void

    /// `height` is the rulebook door's side, whatever it is at this moment of
    /// a resize: the two stand side by side and are the same height (owner).
    public init(title: String, height: CGFloat = UtttRulebookButton.expandedSide,
                act: @escaping () -> Void) {
        self.title = title
        self.height = height
        self.act = act
    }

    /// ABOVE THIS HEIGHT THE DRAWER IS EXPANDED. Messages hands the compact
    /// drawer 340 points at the most (323 with the keyboard up) and the
    /// expanded one 541 at the least (an SE), so anything between is a drawer
    /// in motion, and 440 splits it.
    public static let expandedFrom: CGFloat = 440

    /// The words on a door, or nil for no door. The kernel's.
    public static func title(_ door: Uttt.Door) -> String? {
        switch door {
        case .none:  return nil
        case .again: return Uttt.say(.doorAgain)
        }
    }

    public var body: some View {
        Button(action: act) {
            Text(title)
                .font(.system(size: 14, weight: .bold))
                .tracking(0.56)                         // .04em
                .foregroundStyle(UtttInk.doorInk)
                .frame(maxWidth: .infinity)
                .frame(height: height)
                .background(
                    Canvas { ctx, size in
                        UtttBoard.fill(Uttt.door(w: size.width, h: size.height),
                                       into: ctx, size: size)
                    }
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
    }
}
