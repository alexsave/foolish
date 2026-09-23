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
            UtttDrawerSheet { size, from in sheet(size, from: from) }
        }
    }

    /// docs/UI.html 02: the words and the empty board. ONE LAYOUT AT EVERY
    /// HEIGHT, the play surface's (`Uttt.sheet`): the board's centre is the
    /// sheet's centre and it scales with the drawer, compact to expanded,
    /// with the words at the top left and the board clear of them - beside
    /// them when it is small enough, under them otherwise. There were two
    /// layouts here and a switch at 440 points, so a drag across it jumped
    /// the board from beside the words to under them, 198 to 377 points in
    /// one frame (measured with the ruler), and the strip's board sat 80
    /// points right of centre.
    private func sheet(_ size: CGSize, from: CGFloat?) -> some View {
        let L = Uttt.sheet(.wait, size: size)
        let at = { (s: CGFloat) -> UtiSheet in
            Uttt.sheet(.wait, size: CGSize(width: size.width, height: size.height + s))
        }
        let B = from.map { Uttt.sheet(.wait, size: CGSize(width: size.width, height: $0)) } ?? L
        return ZStack(alignment: .topLeading) {
            if stance != .unreadable {
                board
                    .boardRide(L, at: at)
            } else {
                Color.clear
            }
        }
        .overlay(alignment: .topLeading) {
            /* THE WORDS GO WHERE THE BOARD LEAVES ROOM: in the column beside
             * it on the strip, wrapped onto as many lines as that takes (the
             * board no longer shrinks under them), and across the top once
             * the sheet opens, where UI.html 02 sets them. */
            words(column: true)
                .inWords(L, alignment: .topLeading)
                .wordsRide(column: true, L, from: B, at: at)
            words(column: false)
                .inBand(B, alignment: .topLeading)
                .wordsRide(column: false, L, from: B, at: at)
        }
    }

    private func words(column: Bool) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(headline)
                .font(.system(size: 21, weight: .bold))
                .tracking(-0.315)
                .foregroundStyle(UtttInk.ink)
                .wordsWrap(headline, column: column)
            Text(subline)
                .font(.system(size: 14))
                .foregroundStyle(UtttInk.muted)
                .wordsWrap(subline, column: column)
        }
    }

    /* THE EMPTY BOARD, with no wash: nobody is on move, and a board tinted
     * corner to corner reads as a different piece of paper. */
    private var board: some View {
        UtttBoard(active: -1, last: -1, positionKey: 0)
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

    /// The rules open on the same sheet, as they do on the play surface:
    /// a spectator can read them too.
    @State private var rulesOpen = false


    public var body: some View {
        UtttSheet {
            if rulesOpen {
                UtttRulesSheet { rulesOpen = false }
                    .padding(13)
                    .transition(.opacity)
            } else {
                UtttDrawerSheet { size, _ in watch(size) }
            }
        }
        .animation(.easeInOut(duration: 0.18), value: rulesOpen)
    }

    /// The play surface's one layout (`Uttt.sheet`), with the header line in
    /// place of the bar: the board centred and scaled with the drawer, the
    /// rulebook in the right column on the strip and beside Again at the
    /// bottom when expanded. There was a switch at 440 points here too.
    private func watch(_ size: CGSize) -> some View {
        let L = Uttt.sheet(.watch, size: size)
        let hpad = CGFloat(L.hpad), vpad = CGFloat(L.vpad)
        return UtttBoard(active: model.active, last: model.last,
                         positionKey: model.positionKey)
            .boardRide(L) { s in
                Uttt.sheet(.watch, size: CGSize(width: size.width, height: size.height + s))
            }
            .overlay(alignment: .topLeading) {
                /* On the strip the label over the line in the left column,
                 * the line wrapped; opening, the two across the top band. */
                let label = Text(Uttt.say(.watchLabel))
                    .font(.system(size: 9.5, weight: .semibold))
                    .tracking(1.9)
                    .textCase(.uppercase)
                    .foregroundStyle(UtttInk.label)
                let said = Text(line)
                    .font(.system(size: 17, weight: .bold))
                    .foregroundStyle(UtttInk.ink)
                    .accessibilityAddTraits(.isHeader)
                ZStack(alignment: .topLeading) {
                    VStack(alignment: .leading, spacing: 3) {
                        label.lineLimit(1).minimumScaleFactor(0.5)
                        said.wordsWrap(line, column: true)
                    }
                    .inWords(L, alignment: .topLeading)
                    HStack(alignment: .firstTextBaseline) {
                        label
                        Spacer()
                        said
                    }
                    .inBand(L, alignment: .top)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                /* Again belongs to the expanded view (UI.html 08); the
                 * rulebook stands beside it at its height, as on the play
                 * surface, and alone in the right column on the strip. */
                HStack(alignment: .center, spacing: 10) {
                    if let title = UtttDoorButton.title(door), L.door_alpha > 0 {
                        UtttDoorButton(title: title, height: CGFloat(L.door), act: onDoor)
                            .opacity(Double(L.door_alpha))
                    }
                    UtttRulebookButton(side: CGFloat(L.door)) { rulesOpen = true }
                }
                .padding(.trailing, hpad)
                .padding(.bottom, vpad)
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
