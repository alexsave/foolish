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

#if DEBUG
    /// Cleared on every activation, so each opened bubble asks again.
    private var seatChosen = false
#endif

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
#if DEBUG
        seatChosen = false
#endif
        present(conversation)
    }

    /// A DIFFERENT BUBBLE WAS TAPPED while we were already up. The cold case
    /// arrives through `willBecomeActive` instead, with the same URL on the
    /// conversation, so both ends route through `present` and there is only
    /// one adoption path.
    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        present(conversation)
    }

    /// A move from the other player, which does NOT become the selection.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        present(conversation)
    }

    /// The draft was deleted, so the board it held never existed.
    ///
    /// ONLY IF THIS IS THE DRAFT. Staging a replacement - which is what
    /// tapping a different square does - REPLACES the bubble in the input
    /// field, and Messages reports the replaced one as cancelled, after the
    /// successor has already been recorded. A handler that believed every
    /// cancel would take back the move the player had just decided on. The
    /// host app hit this too and its note is the one worth reading.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        guard message.url == draftURL else { return }
        draftURL = nil
        live?.setPending(false)
        /* The X IS the undo: there is no other button, and leaving the move
         * played would put the board a ply ahead of every bubble in the
         * thread. */
        if Uttt.plyCount > 0, Uttt.undo() {
            staged = staged.map { UtttWire(seed: $0.seed, creator: $0.creator,
                                           joiner: $0.joiner, code: Uttt.code) }
            reverted = true
            live?.refresh()
        } else {
            staged = nil
        }
        present(conversation)
    }

    /// A REVERT BEATS THE SELECTION FOR ONE PRESENT. `insert()` made the
    /// staged bubble the selection and a cancel does not always take that
    /// back, so the ordinary "more plies wins" rule would route the surface
    /// straight back at the bubble the human just discarded - the opposite of
    /// an undo.
    private var reverted = false

    private func present(_ conversation: MSConversation) {
        bag.removeAll()

#if DEBUG
        /* ASKED ONCE PER OPENING, and then never seen again. One phone cannot
         * hold two participants, so with `dev.picker` set the first thing an
         * opened bubble does is ask which of the two people is holding it -
         * and after that the game plays exactly as it would on two phones,
         * with no debug anything on any screen. */
        if UtttDev.picker, !seatChosen {
            show(UtttSeatChoice { [weak self] word in
                guard let self else { return }
                UtttDev.setSeat(word)
                self.seatChosen = true
                if let c = self.activeConversation ?? self.staged.map({ _ in conversation }) {
                    DispatchQueue.main.async { self.present(c) }
                }
            })
            return
        }
#endif

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

        /* OPENING THE APP IS THE INVITATION. There is no "Send a board"
         * button any more: coming in through the + menu with no bubble to
         * read is somebody saying they want a game, so the board goes into
         * the input field there and then and the drawer stays COMPACT with
         * what it has just done on it. A door that asks a second time is a
         * door in the way. */
        guard let wire = current(UtttWire.read(conversation.selectedMessage?.url)) else {
            start(in: conversation)
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
            /* You put the board down and nobody has picked it up. NO MARK ON
             * THIS SCREEN: which seat you have is not decided until the other
             * chair is filled, and a waiting screen that showed one would be
             * telling you what you would get if you re-rolled. */
            show(UtttLobbyScreen(stance: .waiting(nil), seed: wire.seed, act: {}))

        case .some(let seat):
            guard let mark = wire.mark(of: seat) else {
                show(UtttLobbyScreen(stance: .waiting(nil), seed: wire.seed, act: {}))
                return
            }
            showBoard(wire, mark: mark, claiming: nil)

        case .none where !wire.isSealed:
            /* OPENING THE BOARD IS TAKING THE SEAT. There was a screen here
             * that said "there is a seat" over a button that said "take it",
             * which is a door in front of a door: you tapped the bubble, so
             * you want the game. The roster seals, the claim goes into the
             * input field, and the board is what you are looking at. */
            join(wire, as: me)

        case .none:
            // THE ROSTER SEALED AT TWO. This is a group chat and you are not
            // in this game.
            showWatching(wire)
        }
    }

    /// The draft beats the transcript, but only for the same game: tapping an
    /// older bubble, or a different game's, has to win.
    private func current(_ selected: UtttWire?) -> UtttWire? {
        if reverted { reverted = false; return staged }
        guard let staged else { return selected }
        guard let selected else { return staged }
        guard staged.isSameGame(as: selected) else { return selected }
        return staged.plies() >= selected.plies() ? staged : selected
    }

    // MARK: the three things a person can do

    /// Put an empty board on the table. THIS MOMENT IS THE SEED, and every
    /// bubble in the game carries it from here on.
    private func start(in conversation: MSConversation) {
        /* THE CONVERSATION IS THE ARGUMENT, not `activeConversation`. Inside
         * `willBecomeActive(with:)` the property is not set yet, so a
         * `guard let conversation = activeConversation` here returned quietly
         * and the drawer came up empty with no bubble and no error - which
         * looks exactly like a crashed extension. */
        let seed = UtttWire.seedNow()
        let me = UtttWire.tag(participant: conversation.localParticipantIdentifier,
                              seed: seed)
        let wire = UtttWire.opening(seed: seed, creator: me)
        stage(wire, in: conversation)
    }

    /// Take the second seat. THE ROSTER SEALS HERE.
    /// THE ROSTER SEALS HERE, and only here does anybody learn a seat: the
    /// marks come from both tags, so the second one has to exist first.
    private func join(_ wire: UtttWire, as me: String) {
        wire.load()
        let sealed = wire.staging(joining: me)
        guard let mine = sealed.mark(of: .joiner) else { return }

        /* AND ONLY IF THERE IS NOTHING ELSE TO SAY. Sitting down has to be
         * SENT - the other player cannot see a seat that was never sent - but
         * if the draw makes you X then your move is the next thing that
         * happens anyway, and the claim and the move belong in one bubble.
         * Staging an empty board first would put a message in the thread
         * whose only content is "I am here", immediately followed by the one
         * that says it better. */
        if Uttt.over == .none, Uttt.turn == mine {
            showBoard(wire, mark: mine, claiming: me)
        } else {
            stage(sealed, andShowIt: false)
            showBoard(wire, mark: mine, claiming: me)
        }
    }

    /// Seal the position the kernel is holding into the input field.
    ///
    /// `andShowIt` is false for a move, which was made on a board that is
    /// already showing the result of it, and true for everything else - where
    /// the screen that was tapped is not the screen that should follow.
    /// ONE MSSession PER GAME, which is two things at once.
    ///
    /// Messages collapses every older bubble of a session down to its caption
    /// and keeps only the newest interactive, so a twenty-six move game is
    /// one live board in the transcript instead of twenty-six - which is what
    /// the thread wants anyway.
    ///
    /// And a message in a session is a message Messages will hand back:
    /// without one, tapping our own sent bubble opened the extension EXPANDED
    /// (so the tap was routed as a bubble open) with `selectedMessage` nil,
    /// and the app had no way to know which game had been tapped. The host
    /// app has always set one; this is the same answer.
    private var session: MSSession?
    private var sessionGame: UtttWire?

    private func sessionFor(_ wire: UtttWire, _ conversation: MSConversation) -> MSSession {
        if let s = session, let g = sessionGame, g.isSameGame(as: wire) { return s }
        // A bubble we are continuing carries its own; a brand new game gets a
        // brand new one, or the last game's final board folds into it.
        let s = conversation.selectedMessage?.session ?? MSSession()
        session = s; sessionGame = wire
        return s
    }

    private func stage(_ wire: UtttWire, andShowIt: Bool = true,
                       in conv: MSConversation? = nil) {
        guard let conversation = conv ?? activeConversation else { return }
        let message = MSMessage(session: sessionFor(wire, conversation))
        message.url = wire.url
        message.layout = layout(for: wire)
        /* THE COLLAPSED LINE IS OURS TOO. A session folds every older bubble
         * down to one grey row, and without this Messages writes that row
         * itself - "+1 (555) 564-8583 sent Ultimate message", a phone number
         * and an app's name, in a thread where every other line is about a
         * board. */
        message.summaryText = wire.isSealed && Uttt.plyCount == 0
            ? UtttBubble.sealedCaption : UtttBubble.caption
        staged = wire
        draftURL = message.url
        live?.setPending(true)
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

    /// The board on screen, so a cancel can tell it the draft is gone.
    private weak var live: UtttModel?

    /// The draft currently in the input field. Messages reports a REPLACED
    /// bubble as cancelled, so a cancel that does not name this one is stale.
    private var draftURL: URL?

    private func showBoard(_ wire: UtttWire, mark: Uttt.Mark, claiming: String?) {
        /* ONE GAME LIVES IN THE KERNEL and UtttModel's init starts a new one
         * on it, so the position goes in AFTER the model exists and the screen
         * is told to look again. Any other order shows an empty board over a
         * game in progress. */
        let model = UtttModel(seed: wire.seed, you: mark)
        wire.load()
        model.refresh()
        live = model

        /* The model does not know there is a conversation and should not. It
         * says the position changed; a position that changed is a move this
         * device made, because the kernel will not let it move out of turn. */
        model.$positionKey
            .dropFirst()
            .sink { [weak self] _ in
                guard let self else { return }
                /* A REPLACEMENT RESTAGES, it does not stage a second bubble.
                 * Uttt.plyCount going DOWN is the undo half of a change of
                 * mind, and there is nothing to put in the field for it - the
                 * move that replaces it arrives a beat later and stages then.
                 * Without this the input field briefly carries the position
                 * the player just rejected. */
                guard Uttt.turn != mark || Uttt.over != .none else { return }
                self.stage(wire.staging(joining: claiming), andShowIt: false)
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

        /* A CONSTANT, NOT A SEARCH - and not the bot, which is research and
         * has no business inside the app. These are the first moves of the
         * game the render harness draws, so a seeded screenshot here and a
         * PPM from `make render` are the same board. */
        Uttt.newGame(seed: seed)
        let opening = [34, 67, 44, 80, 76, 43, 69, 62, 79, 63, 4, 40, 39, 31,
                       37, 16, 70, 71, 72, 3, 29, 19, 17, 73, 14, 50, 45, 6]
        for mv in opening.prefix(max(0, plies)) where Uttt.over == .none {
            _ = Uttt.play(mv)
        }
        seatSeeded(UtttWire(seed: seed, creator: a, joiner: b, code: Uttt.code))
    }

    /// Open a seeded game from whichever chair `dev.seat` is sitting in.
    private func seatSeeded(_ wire: UtttWire) {
        let me = UtttWire.tag(participant: UUID(), seed: wire.seed)
        let seat = wire.seat(of: me) ?? .creator
        guard let mark = wire.mark(of: seat) else { return }
        showBoard(wire, mark: mark, claiming: nil)
    }
#endif

    private func showWatching(_ wire: UtttWire) {
        let model = UtttModel(seed: wire.seed, you: .none)
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
    private func layout(for wire: UtttWire) -> MSMessageLayout {
        if wire.isSealed, Uttt.plyCount == 0 {
            let l = MSMessageTemplateLayout()
            l.image = UtttBubble.image()
            l.caption = UtttBubble.sealedCaption
            return l
        }
        return UtttBubble.layout()
    }
}
