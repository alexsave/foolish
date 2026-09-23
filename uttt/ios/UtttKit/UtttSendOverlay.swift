import SwiftUI

/// WHAT STANDS OVER EVERY SCREEN while a bubble is on its way out: the send
/// hint and, when Messages never answered an insert, the send door.
///
/// It lives over the screens rather than in them because the extension swaps
/// whole screens (a move that ends the game swaps the board for the finished
/// one, Again swaps it for the lobby) and the bubble in the field does not
/// care which screen is up - the hint's fuse must keep burning across a swap,
/// which it can only do if nothing under it is torn down.
public final class UtttSendState: ObservableObject {
    /// A bubble is in the input field and nobody has sent it.
    @Published public var staged = false
    /// Bumped by every stage, so a replacement bubble restarts the fuse.
    @Published public var restart = 0
    /// The drawer is compact: the only place Messages' Send button is above us.
    @Published public var compact = true
    /// Every insert went unanswered; the door inserts on a tap.
    @Published public var door = false

    public init() {}
}

public struct UtttSendOverlay: View {
    @ObservedObject var state: UtttSendState
    let onDoor: () -> Void

    public init(state: UtttSendState, onDoor: @escaping () -> Void) {
        self.state = state
        self.onDoor = onDoor
    }

    /// The hint's container starts this far below the drawer's top, the
    /// sister product's board inset: the arrow rests in the top margin and
    /// its crest reaches up toward Messages' Send button (SendHintArrow.crestRoom).
    static let hintTop: CGFloat = 14

    /// The band at the drawer's bottom the send door stands in: the door and
    /// the paper around it. The only part of the overlay that takes a touch.
    public static let doorStrip: CGFloat = UtttRulebookButton.expandedSide + 2 * doorPad
    static let doorPad: CGFloat = 10

    public var body: some View {
        ZStack {
            // THE HINT, verbatim from the sister product (shared/swift/MessagesKit):
            // the same arrow, blue, bob, ring, fade and axis. What is ours is the
            // caption and the fuse, both the kernel's.
            SendHint(staged: state.staged && !state.door, visible: state.compact,
                     caption: Uttt.say(.sendHint),
                     fuse: Uttt.sendHintSeconds, restart: state.restart)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                .padding(.top, Self.hintTop)

            // THE SEND DOOR: every insert went unanswered, so the bubble never
            // reached the field. A tap inserts it again - by then the drawer is
            // presenting and Messages' gate lets it through
            // (docs/INSERT_GATING.md). The Again door's pen and type, on a
            // strip of paper so it reads over whatever board is under it.
            if state.door {
                VStack {
                    Spacer(minLength: 0)
                    UtttDoorButton(title: Uttt.say(.doorSend), act: onDoor)
                        .padding(.horizontal, 16)
                        .padding(.vertical, Self.doorPad)
                        .background(Color(uiColor: UtttPaper.flat))
                }
                .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.25), value: state.door)
    }
}
