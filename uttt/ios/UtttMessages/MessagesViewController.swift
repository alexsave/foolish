import Combine
import Messages
import SwiftUI
import UtttKit

/// The extension. It owns the conversation and nothing else - every rule and
/// every coordinate is the kernel's, the screens are UtttKit's, and what a
/// bubble says is `UtttWire`'s.
///
/// THERE IS NO CHAIN TO WALK. An extension is handed exactly one message, the
/// one that was tapped, and cannot enumerate the transcript - so this file
/// never looks for an earlier bubble. Everything it needs is in front of it.
final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<AnyView>?
    private var bag = Set<AnyCancellable>()

    /// The bubble sitting in the input field, which nobody has sent yet.
    ///
    /// Messages can re-present the extension while a draft is waiting on the
    /// arrow, and the tapped message is then one move behind the board the
    /// player is looking at. Without this, that re-read walks the board
    /// backwards under a move they have already made.
    private var staged: UtttWire?

    // MARK: the conversation

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        present(conversation)
    }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        present(conversation)
    }

    /// The draft was deleted, so the board it held never existed.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        staged = nil
        present(conversation)
    }

    private func present(_ conversation: MSConversation) {
        bag.removeAll()

#if DEBUG
        /* STRAIGHT TO THE BOARD. One simulator cannot play a two-handed game
         * in a transcript - see UtttDev - so with `dev.game` set the lobby,
         * the invitation and the tap on a bubble are all skipped and the
         * position is built here. Absent in every ordinary run, including an
         * ordinary DEBUG one, and gone entirely from a shipping build. */
        if conversation.selectedMessage == nil, staged == nil,
           let plies = UtttDev.game {
            showSeeded(plies)
            return
        }
#endif

        guard let wire = current(UtttWire.read(conversation.selectedMessage?.url)) else {
            show(UtttLobbyScreen(stance: .start) { [weak self] in self?.start() })
            return
        }
        staged = wire

        guard wire.load() else {
            show(UtttLobbyScreen(stance: .unreadable, act: {}))
            return
        }

        /* WHICH SEAT IS THIS DEVICE'S. A tag only ever answers "is this me?",
         * and the device asking is the one that wrote it - see UtttWire.tag.
         * Everybody else in a group chat matches neither, which is how a third
         * tap becomes a spectator instead of a third player. */
        let me = UtttWire.tag(participant: conversation.localParticipantIdentifier,
                              seed: wire.seed)

        switch wire.seat(of: me) {
        case .some(.creator) where !wire.isSealed:
            // You put the board down. Nobody has picked it up, and X cannot
            // open against an empty chair.
            show(UtttLobbyScreen(stance: .waiting(wire.mark(of: .creator)),
                                 seed: wire.seed, act: {}))

        case .some(let seat):
            showBoard(wire, mark: wire.mark(of: seat), claiming: nil)

        case .none where !wire.isSealed:
            let mine = wire.mark(of: .joiner)
            show(UtttLobbyScreen(stance: .open(mine), seed: wire.seed) {
                [weak self] in self?.join(wire, as: me)
            })

        case .none:
            // THE ROSTER SEALED AT TWO. This is a group chat and you are not
            // in this game.
            showWatching(wire)
        }
    }

    /// The draft beats the transcript, but only for the same game: tapping an
    /// older bubble, or a different game's, has to win.
    private func current(_ selected: UtttWire?) -> UtttWire? {
        guard let staged else { return selected }
        guard let selected else { return staged }
        guard staged.isSameGame(as: selected) else { return selected }
        return staged.plies() >= selected.plies() ? staged : selected
    }

    // MARK: the three things a person can do

    /// Put an empty board on the table. THIS MOMENT IS THE SEED, and every
    /// bubble in the game carries it from here on.
    private func start() {
        guard let conversation = activeConversation else { return }
        let seed = UtttWire.seedNow()
        let me = UtttWire.tag(participant: conversation.localParticipantIdentifier,
                              seed: seed)
        let wire = UtttWire.opening(seed: seed, creator: me)
        stage(wire, actor: wire.mark(of: .creator))
    }

    /// Take the second seat. THE ROSTER SEALS HERE.
    private func join(_ wire: UtttWire, as me: String) {
        let mine = wire.mark(of: .joiner)
        wire.load()
        if Uttt.over == .none, Uttt.turn == mine {
            /* Your turn the moment you sit down, so the seat claim and the
             * opening move are one bubble. The thread pays for one message
             * instead of two, and nothing else changes. */
            showBoard(wire, mark: mine, claiming: me)
        } else {
            // Not your turn, and there is still nothing to do but say so out
            // loud - the other player cannot see a seat that was never sent.
            stage(wire.staging(joining: me), actor: mine)
        }
    }

    /// Seal the position the kernel is holding into the input field.
    ///
    /// `andShowIt` is false for a move, which was made on a board that is
    /// already showing the result of it, and true for everything else - where
    /// the screen that was tapped is not the screen that should follow.
    private func stage(_ wire: UtttWire, actor: Uttt.Mark, andShowIt: Bool = true) {
        guard let conversation = activeConversation else { return }
        let message = MSMessage()
        message.url = wire.url
        message.layout = layout(for: wire, actor: actor)
        staged = wire
#if DEBUG
        /* The seeded game's state, so the other seat finds this move. */
        if UtttDev.game != nil { UtttDev.live = wire.url.absoluteString }
#endif
        conversation.insert(message) { _ in }

        /* AN EXTENSION CANNOT SEND. insert() only puts the bubble in the input
         * field; the arrow is the human's. So the surface gets out of the way
         * of the thing it has just asked them to tap. */
        requestPresentationStyle(.compact)

        /* And it says what it now is. Messages does not re-present an already
         * compact extension, so without this the door that has just been used
         * is still standing there offering to do the same thing again.
         *
         * NEXT TURN OF THE RUNLOOP, because the thing being replaced is the
         * view whose button is still in the middle of calling this - tearing
         * it down under itself leaves an empty drawer. */
        if andShowIt {
            DispatchQueue.main.async { [weak self] in self?.present(conversation) }
        }
    }

    // MARK: the screens

    private func showBoard(_ wire: UtttWire, mark: Uttt.Mark, claiming: String?) {
        /* ONE GAME LIVES IN THE KERNEL and UtttModel's init starts a new one
         * on it, so the position goes in AFTER the model exists and the screen
         * is told to look again. Any other order shows an empty board over a
         * game in progress. */
        /* NAMED BY THEIR MARK, because that is the only name this side has.
         * A Messages extension cannot resolve a participant's display name -
         * it gets a per-device UUID - so until a nickname travels on the wire,
         * "O" is the true answer and "nib" was a bot from the demo. The
         * bubble's own caption already speaks this vocabulary ("X started a
         * game."), so the two surfaces agree. */
        let model = UtttModel(seed: wire.seed, you: mark, solo: false,
                              opponent: mark == .x ? "O" : "X")
        wire.load()
        model.refresh()

        /* The model does not know there is a conversation and should not. It
         * says the position changed; a position that changed is a move this
         * device made, because the kernel will not let it move out of turn. */
        model.$positionKey
            .dropFirst()
            .sink { [weak self] _ in
                self?.stage(wire.staging(joining: claiming), actor: mark,
                            andShowIt: false)
            }
            .store(in: &bag)

        show(UtttGameScreen(model: model))
    }

#if DEBUG
    /// A game `plies` moves in, both seats taken, seated as `dev.seat` says.
    private func showSeeded(_ plies: Int) {
        let seed = UtttDev.seed
        let a = UtttWire.tagForDev("a", seed: seed)
        let b = UtttWire.tagForDev("b", seed: seed)

        /* WHERE THE GAME ACTUALLY IS, if anybody has moved. Rebuilding the
         * opening here would undo the other seat's move every time the seat
         * flipped, and the board would never leave ply `plies`. */
        if let live = UtttDev.live, let wire = UtttWire.read(URL(string: live)),
           wire.seed == seed, wire.load() {
            seatSeeded(wire)
            return
        }

        Uttt.newGame(seed: seed)
        for _ in 0..<max(0, plies) {
            guard Uttt.over == .none else { break }
            let mv = Uttt.botMove(budget: 40)
            guard mv >= 0, Uttt.play(mv) else { break }
        }
        seatSeeded(UtttWire(seed: seed, creator: a, joiner: b, code: Uttt.code))
    }

    /// Open a seeded game from whichever chair `dev.seat` is sitting in.
    private func seatSeeded(_ wire: UtttWire) {
        let me = UtttWire.tag(participant: UUID(), seed: wire.seed)
        let seat = wire.seat(of: me) ?? .creator
        showBoard(wire, mark: wire.mark(of: seat), claiming: nil)
    }
#endif

    private func showWatching(_ wire: UtttWire) {
        let model = UtttModel(seed: wire.seed, you: .none, solo: false)
        wire.load()
        model.refresh()
        show(UtttWatchScreen(model: model))
    }

    private func show<V: View>(_ screen: V) {
        host?.willMove(toParent: nil)
        host?.view.removeFromSuperview()
        host?.removeFromParent()

        let vc = UIHostingController(rootView: AnyView(screen))
        addChild(vc)
        vc.view.frame = view.bounds
        vc.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        vc.view.backgroundColor = .clear
        view.addSubview(vc.view)
        vc.didMove(toParent: self)
        host = vc
    }

    // MARK: the bubble

    /// The face of the message is `UtttBubble`'s and the kernel's. The only
    /// thing decided here is who the sentence is about - and in this game the
    /// only name anybody has is their mark, which is the one name that reads
    /// the same on both phones.
    private func layout(for wire: UtttWire, actor: Uttt.Mark) -> MSMessageLayout {
        let name = actor == .o ? "O" : "X"
        if wire.isSealed, Uttt.plyCount == 0 {
            // A seat claim: the one bubble that is neither a move nor the
            // invitation, and the one caption UtttBubble has no case for.
            let l = MSMessageTemplateLayout()
            l.image = UtttBubble.image()
            l.caption = "\(name) took the other side."
            return l
        }
        return UtttBubble.layout(actor: name)
    }
}
