import Combine
import Messages
import SwiftUI
import UtttKit

/// The extension. It owns the conversation and nothing else - every rule and
/// every coordinate is the kernel's, the screens are UtttKit's, and what a
/// bubble carries, who sits where, what they may do and which door they get
/// is `uttt_msg.h`'s.
///
/// THERE IS NO CHAIN TO WALK. An extension is handed exactly one message, the
/// one that was tapped, and cannot enumerate the transcript - so this file
/// never looks for an earlier bubble. Everything it needs is in front of it.
///
/// THE MESSAGES IT HOLDS, and the kernel ranks them (utm_prefer):
///   - the tapped one: what Messages says is selected, or what just arrived;
///   - `staged`: the draft in the input field, which nobody has sent;
///   - `sent`: the last bubble this device sent.
/// The draft and the sent bubble are this device's own newest; the tapped one
/// is everybody else's. Which of them the screen shows is one kernel call.
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

    /// The last bubble this device SENT (didStartSending). After the arrow the
    /// draft is gone from the field, but the board must not go back to the
    /// bubble that was tapped before it - which can still be the selection.
    /// foolish's `markSent`, with its rule: it never rebases backwards.
    private var sent: UtttWire?

    /// A bubble that ARRIVED while we were up (didReceive). It is not the
    /// selection, so without this the next present would read the old one.
    private var arrived: UtttWire?

    /// One present only: the position a cancelled draft reverted to. The
    /// cancelled bubble can still be the selection, and "more plies wins"
    /// would route straight back at the move the human just discarded.
    private var reverted: UtttWire?

    /// THE DRAFT IS A NEW GAME, so it beats the selection even though the
    /// kernel ranks a different game's tapped bubble first. Set by Again,
    /// whose finished game is still the selection; cleared by the next tap.
    private var draftIsNewGame = false

    /// The draft currently in the input field. Messages reports a REPLACED
    /// bubble as cancelled, so a cancel that does not name this one is stale.
    private var draftURL: URL?

    /// A fresh MSSession for the next stage: Again starts a new game, and a
    /// new game must never fold the finished game's last bubble into itself.
    private var freshSession = false

    /// A drawer opened from the + menu is bound to no message at all, and the
    /// host delivers nothing to it - no didReceive, no didSelect - until a
    /// bubble is tapped. foolish established this from the host binaries (see
    /// its didStartSending); the answer is the same here: the first send from
    /// an unbound drawer closes it, so the next thing the human does - tap
    /// the bubble - binds it.
    private var unbound = true

    // MARK: the conversation

#if DEBUG
    /// Cleared on every activation and every bubble tapped while up, so each
    /// opened bubble asks again.
    private var seatChosen = false
#endif

    override func viewDidLoad() {
        super.viewDidLoad()
        UtttLog.note("load")
        /* CLEAR UNTIL IT APPEARS - see `appeared`. */
        view.backgroundColor = .clear
    }

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        UtttLog.note("active", "\(styleName), selected \(conversation.selectedMessage != nil)")
        becameActiveAt = Date()
        arrived = nil
        draftIsNewGame = false
        unbound = conversation.selectedMessage == nil
#if DEBUG
        seatChosen = false
#endif
        present(conversation)
    }

    /// NOTHING IS DRAWN UNTIL THE DRAWER HAS A SIZE.
    ///
    /// Measured on a cold open (log lines `layout`, filmed alongside): the
    /// extension's view is first laid out at the WHOLE WINDOW - 440 by 956 on
    /// a Pro Max - and Messages puts it on screen at that size for about half
    /// a second before the compact transition shrinks it to 309. Whatever
    /// this view drew then covered the whole thread, status bar and all: a
    /// full-screen sheet of paper flashing in before the drawer, and a board
    /// rasterised at 414 points only to be thrown away for 207.
    ///
    /// So until viewDidAppear - by which point the drawer is its real size -
    /// the screen is built but not attached, and the view stays clear, which
    /// shows Messages' own drawer card. On appearing, the newest screen goes
    /// in and paper is painted under it, and whatever was waiting for a real
    /// drawer (the invitation's insert) runs.
    private var appeared = false
    private var pendingScreen: AnyView?
    private var afterAppear: [() -> Void] = []

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UtttLog.note("appear", "\(Int(view.bounds.width))x\(Int(view.bounds.height))")
        appeared = true
        view.backgroundColor = UtttPaper.flat
        if let screen = pendingScreen {
            pendingScreen = nil
            attach(screen)
        }
        let work = afterAppear
        afterAppear.removeAll()
        work.forEach { $0() }
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        appeared = false
    }

    /// Run `work` once the drawer is on screen at its real size.
    private func whenAppeared(_ work: @escaping () -> Void) {
        if appeared { work() } else { afterAppear.append(work) }
    }

    override func didResignActive(with conversation: MSConversation) {
        super.didResignActive(with: conversation)
        UtttLog.note("resign")
    }

    /// When this activation began. A tap that launches or re-activates the
    /// extension brings its own selection WITH it, and willBecomeActive
    /// handles that; a selection that moves while we are already up is a
    /// different bubble tapped (or our own insert).
    private var becameActiveAt: Date?
    private var freshlyActive: Bool {
        guard let t = becameActiveAt else { return false }
        return Date().timeIntervalSince(t) < 1
    }

    /// A DIFFERENT BUBBLE WAS TAPPED while we were already up.
    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        super.didSelect(message, conversation: conversation)
        /* OUR OWN INSERT MOVES THE SELECTION TOO. That is not a tap, and
         * re-asking the seat picker over it would ask the person who just
         * moved who they are. */
        if let u = message.url, u == draftURL || u == sent?.url {
            UtttLog.note("select", "own bubble")
            return
        }
        if freshlyActive {
            UtttLog.note("select", "on a fresh activation - willBecomeActive has it")
            return
        }
        UtttLog.note("select", "a bubble tapped while open")
        unbound = false
        arrived = nil
        draftIsNewGame = false
#if DEBUG
        /* A new bubble is a new question: whose hands is it in. */
        seatChosen = false
#endif
        present(conversation)
    }

    /// A move from the other player, which does NOT become the selection.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        super.didReceive(message, conversation: conversation)
        UtttLog.note("receive")
        arrived = UtttWire(url: message.url)
        present(conversation)
    }

    /// THE HUMAN TAPPED THE ARROW: the draft is in the thread now.
    ///
    /// The message Messages hands over is the authority - the bytes that
    /// actually went out - not our own bookkeeping, which a replaced draft or
    /// a torn-down extension can have lost.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        super.didStartSending(message, conversation: conversation)
        let wire = UtttWire(url: message.url) ?? staged
        UtttLog.note("send", wire.map { "\($0.text.count) chars" } ?? "NO PAYLOAD")
        markSent(wire)
        if message.url == draftURL {
            draftURL = nil
            staged = nil
        }
        freshSession = false
        live?.setPending(false)
        let wasUnbound = unbound
        present(conversation)

        /* A SEND FROM THE EXPANDED DRAWER is somebody done with it; a send
         * from the compact one keeps the strip up so the next move is one tap
         * away - foolish's round-16 rule. Except the first bubble of an
         * unbound drawer: nothing will ever arrive at it (see `unbound`). */
        if presentationStyle != .compact {
            UtttLog.note("dismiss", "sent from expanded")
            dismiss()
        } else if wasUnbound {
            UtttLog.note("dismiss", "first send from an unbound drawer")
            dismiss()
        }
    }

    /// markSent NEVER REBASES BACKWARDS. A send whose bytes lose to what this
    /// device already sent in the same game (a stale report, a replaced
    /// draft reported late) changes nothing - adopting it would walk the
    /// board back under a move that is already in the thread.
    private func markSent(_ wire: UtttWire?) {
        guard let wire else { return }
        if let old = sent, old.isSameGame(as: wire),
           !Uttt.prefersMine(wire.text, over: old.text) {
            UtttLog.fault("send", "refused: older than what was already sent")
            return
        }
        sent = wire
    }

    /// The draft was deleted, so the board it held never existed.
    ///
    /// ONLY IF THIS IS THE DRAFT. Staging a replacement - which is what
    /// tapping a different square does - REPLACES the bubble in the input
    /// field, and Messages reports the replaced one as cancelled, after the
    /// successor has already been recorded.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        super.didCancelSending(message, conversation: conversation)
        guard message.url == draftURL, let draft = staged else {
            UtttLog.note("cancel", "a replaced draft - ignored")
            return
        }
        UtttLog.note("cancel", "the draft")
        draftURL = nil
        staged = nil
        stageGeneration += 1           // a stage still waiting to insert is void
        live?.setPending(false)

        /* THE X IS THE UNDO, and what comes back is the kernel's rule: only my
         * own move (or my own take-back), and taking back a joining move
         * gives the seat back. The draft is re-read first, because the
         * resident message is whatever was read last. */
        identify(conversation)
        guard draft.load(), Uttt.undoMine(), let back = UtttWire.resident else {
            /* AN INVITATION NOBODY SENT, taken out of the field: there is no
             * game left to show, and staying up would stage a new one at once.
             * Close, and the human is back at their keyboard. */
            UtttLog.note("dismiss", "the invitation draft was cancelled")
            dismiss()
            return
        }
        reverted = back
#if DEBUG
        /* The seeded game's state goes back with it, or the next open of the
         * seeded board would read the cancelled move back out of dev.live. */
        if UtttDev.game != nil { UtttDev.live = back.text }
#endif
        present(conversation)
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
        UtttLog.note("will-style", Self.name(presentationStyle))
    }

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
        UtttLog.note("style", Self.name(presentationStyle))
        let waiters = transitionWaiters
        transitionWaiters.removeAll()
        for seq in waiters.keys.sorted() { waiters[seq]?.resume() }
    }

    private static func name(_ s: MSMessagesAppPresentationStyle) -> String {
        switch s {
        case .compact:    return "compact"
        case .expanded:   return "expanded"
        case .transcript: return "transcript"
        @unknown default: return "other"
        }
    }
    private var styleName: String { Self.name(presentationStyle) }

    // MARK: routing

    private func present(_ conversation: MSConversation) {
        bag.removeAll()

#if DEBUG
        /* ASKED ONCE PER OPENED BUBBLE, and then never seen again. One phone
         * cannot hold two participants, so with `dev.picker` set the first
         * thing an opened bubble does is ask which of the two people is
         * holding it. */
        if UtttDev.picker, !seatChosen {
            show(UtttSeatChoice { [weak self] word in
                guard let self else { return }
                UtttDev.setSeat(word)
                self.seatChosen = true
                DispatchQueue.main.async { self.present(conversation) }
            })
            return
        }

        /* STRAIGHT TO THE BOARD with `dev.game` set: see UtttDev. Seated
         * first - the seeded board's "you are" is a seat question too. */
        identify(conversation)
        if conversation.selectedMessage == nil, staged == nil, sent == nil,
           reverted == nil, let plies = UtttDev.game {
            showSeeded(plies, conversation)
            return
        }
#endif

        identify(conversation)

        let tapped = newest(UtttWire(url: conversation.selectedMessage?.url), arrived)
        guard let wire = current(tapped) else {
            /* OPENING THE APP IS THE INVITATION. Coming in through the + menu
             * with no bubble to read is somebody saying they want a game. */
            start(in: conversation)
            return
        }

        guard wire.load() else {
            UtttLog.fault("read", "unreadable bubble")
            show(UtttLobbyScreen(stance: .unreadable))
            return
        }

        let door = Uttt.door(sent: isSent(wire))
        UtttLog.note("present", "seat \(Uttt.seat) plies \(Uttt.plyCount) door \(door)")

        /* WHICH SEAT IS THIS DEVICE'S is the kernel's answer: it hashes this
         * device's participant with the game's seed and looks for the result. */
        switch Uttt.seat {
        case .waiting:
            show(UtttLobbyScreen(stance: .waiting, door: door) { [weak self] in
                self?.takeBack(wire, in: conversation)
            })

        case .closed:
            show(UtttLobbyScreen(stance: .closed))

        case .open, .x, .o:
            /* OPENING SOMEBODY'S INVITATION IS SITTING DOWN AS X, and the
             * first move is yours: the join and the first move are one
             * message, staged when the move is made. */
            showBoard(mark: Uttt.myMark, door: door, conversation)

        case .spectator:
            let model = UtttModel(seed: Uttt.seed, you: .none)
            model.refresh()
            show(UtttWatchScreen(model: model, door: door) { [weak self] in
                self?.again(in: conversation)
            })
        }
    }

    /// Of the selection and an arrival, the one the kernel ranks higher - but
    /// an arrival from a DIFFERENT game never overrides what was tapped.
    private func newest(_ selected: UtttWire?, _ arrival: UtttWire?) -> UtttWire? {
        guard let arrival else { return selected }
        guard let selected else { return arrival }
        guard selected.isSameGame(as: arrival) else { return selected }
        return Uttt.prefersMine(arrival.text, over: selected.text) ? arrival : selected
    }

    /// Whether `wire` is in the thread rather than a draft in the field: the
    /// one fact the kernel's door rule needs from the host.
    private func isSent(_ wire: UtttWire) -> Bool { wire != staged }

    /// WHO THIS DEVICE IS, told to the kernel before every question about a
    /// seat. Messages' participant identifier is per device per conversation,
    /// which is exactly the scope a seat needs.
    private func identify(_ conversation: MSConversation) {
#if DEBUG
        if let word = UtttDev.seat {
            Uttt.me(UtttDev.identity(word))
            return
        }
#endif
        Uttt.me(participant: conversation.localParticipantIdentifier)
    }

    /// This device's newest against what Messages handed over. The kernel
    /// decides, including which of two joiners got the seat; a different game
    /// tapped wins, unless the draft is a new game this device just asked for.
    private func current(_ tapped: UtttWire?) -> UtttWire? {
        if let r = reverted { reverted = nil; return r }
        if draftIsNewGame, let staged { return staged }
        guard let mine = staged ?? sent else { return tapped }
        guard let tapped else { return mine }
        return Uttt.prefersMine(mine.text, over: tapped.text) ? mine : tapped
    }

    // MARK: the things a person can do

    /// Put an empty board on the table. THIS MOMENT IS THE SEED.
    ///
    /// SHOWN FIRST, STAGED ONCE THE DRAWER IS UP. Baking the bubble is the
    /// one slow thing on this path, and on a cold open it used to run before
    /// the first screen existed; and the insert used to land while Messages
    /// was still presenting the drawer, which is when the whole-window flash
    /// was at its longest (see `appeared`).
    private func start(in conversation: MSConversation) {
        Uttt.openInvitation()
        guard let wire = UtttWire.resident else {
            UtttLog.fault("start", "the kernel wrote no invitation")
            return
        }
        UtttLog.note("start")
        staged = wire
        present(conversation)
        whenAppeared { [weak self] in
            DispatchQueue.main.async {
                guard let self, self.staged == wire else { return }
                self.stage(wire, in: conversation)
            }
        }
    }

    /// TAKE IT BACK (docs/UI.html 02). Only offered on a SENT invitation -
    /// the kernel's rule - and it is a message: the take-back goes into the
    /// field in the invitation's own session, and on Send it replaces the
    /// invitation in the transcript.
    private func takeBack(_ wire: UtttWire, in conversation: MSConversation) {
        identify(conversation)
        guard wire.load(), Uttt.takeBack(), let back = UtttWire.resident else {
            UtttLog.fault("take-back", "refused by the kernel")
            return
        }
        UtttLog.note("take-back")
        stage(back, in: conversation)
        present(conversation)
    }

    /// AGAIN (docs/UI.html 06, 07): a fresh invitation from whoever asks, in a
    /// NEW session, so the finished game's last bubble stays in the thread.
    /// Whoever proposes moves second - the lobby rule - which is also what
    /// swaps the players for a rematch.
    private func again(in conversation: MSConversation) {
        UtttLog.note("again")
        identify(conversation)
        freshSession = true
        draftIsNewGame = true
        sent = nil
        arrived = nil
        start(in: conversation)
    }

    // MARK: staging

    /// ONE MSSession PER GAME. Messages collapses every older bubble of a
    /// session down to its caption and keeps only the newest interactive, and
    /// a message in a session is one Messages will hand back when tapped.
    private var session: MSSession?
    private var sessionGame: UtttWire?

    private func sessionFor(_ wire: UtttWire, _ conversation: MSConversation) -> MSSession {
        if !freshSession, let s = session, let g = sessionGame, g.isSameGame(as: wire) { return s }
        let selected = conversation.selectedMessage
        let s: MSSession
        if !freshSession, let sel = selected?.session,
           let w = UtttWire(url: selected?.url), w.isSameGame(as: wire) {
            s = sel
        } else {
            s = MSSession()
        }
        session = s; sessionGame = wire
        return s
    }

    /// Which stage owns the input field. NEWEST STAGE WINS: a stage that has
    /// to wait for the drawer to collapse can be overtaken by a change of
    /// mind, and the older one must then neither record itself nor insert.
    private var stageGeneration = 0

    /// Put `wire` into the input field.
    ///
    /// COLLAPSE FIRST, INSERT AFTER. From the expanded drawer, inserting while
    /// the drawer is still big makes Messages fly the new bubble's preview in
    /// over a drawer that is shrinking under it, and an insert issued in the
    /// middle of a presentation change is the prime suspect for a bubble that
    /// never reached the field. So an expanded stage asks for compact, waits
    /// for didTransition (or a timeout: never hang on a transition Messages
    /// decided not to run), and only then inserts. From compact it inserts at
    /// once. foolish's MessagesViewController.stage, round 10b.
    private func stage(_ wire: UtttWire, in conversation: MSConversation) {
        stageGeneration += 1
        let generation = stageGeneration

        /* BAKED NOW, from the message being staged, before anything can load
         * a different one into the kernel's one resident slot. */
        guard wire.load() else {
            UtttLog.fault("stage", "the kernel cannot read what it wrote")
            return
        }
        let message = MSMessage(session: sessionFor(wire, conversation))
        message.url = wire.url
        message.layout = UtttBubble.layout()
        /* THE COLLAPSED LINE IS OURS TOO, or Messages writes "<phone number>
         * sent Ultimate message" into a thread about a board. */
        message.summaryText = UtttBubble.caption
        freshSession = false

        staged = wire
        draftURL = message.url
        live?.setPending(true)
#if DEBUG
        if UtttDev.game != nil { UtttDev.live = wire.text }
#endif

        if presentationStyle == .compact {
            insert(message, generation: generation, in: conversation, attempt: 1)
            return
        }
        UtttLog.note("stage", "collapsing first")
        requestPresentationStyle(.compact)
        Task { @MainActor [weak self] in
            await self?.awaitTransitionSettled()
            guard let self, self.stageGeneration == generation else {
                UtttLog.note("stage", "overtaken while collapsing")
                return
            }
            self.insert(message, generation: generation, in: conversation, attempt: 1)
        }
    }

    /// NOTHING IS DROPPED SILENTLY. A refused insert is logged with Messages'
    /// own error and tried once more a beat later; if that fails too the
    /// draft is treated exactly as a cancelled one - the board goes back, so
    /// it never shows a move the input field does not hold.
    private func insert(_ message: MSMessage, generation: Int,
                        in conversation: MSConversation, attempt: Int) {
        UtttLog.note("insert", "attempt \(attempt)")
        conversation.insert(message) { [weak self] error in
            DispatchQueue.main.async {
                guard let self else { return }
                guard let error else {
                    UtttLog.note("inserted")
                    return
                }
                UtttLog.fault("insert", "attempt \(attempt) failed: \(error.localizedDescription)")
                guard self.stageGeneration == generation else { return }
                if attempt < 2 {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
                        guard self.stageGeneration == generation else { return }
                        self.insert(message, generation: generation, in: conversation,
                                    attempt: attempt + 1)
                    }
                    return
                }
                UtttLog.fault("insert", "gave up; the draft is reverted")
                self.didCancelSending(message, conversation: conversation)
            }
        }
    }

    private var transitionWaiters: [Int: CheckedContinuation<Void, Never>] = [:]
    private var transitionWaiterSeq = 0

    @MainActor
    private func awaitTransitionSettled(timeoutNs: UInt64 = 1_200_000_000) async {
        transitionWaiterSeq += 1
        let id = transitionWaiterSeq
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            transitionWaiters[id] = c
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: timeoutNs)
                if let waiter = self?.transitionWaiters.removeValue(forKey: id) {
                    UtttLog.note("stage", "no transition came; inserting anyway")
                    waiter.resume()
                }
            }
        }
    }

    // MARK: the screens

    /// The board on screen, so a send or a cancel can tell it about the draft.
    private weak var live: UtttModel?

    /// The board for the message the kernel is holding, as `mark`.
    private func showBoard(mark: Uttt.Mark, door: Uttt.Door, _ conversation: MSConversation) {
        let model = UtttModel(seed: Uttt.seed, you: mark)
        model.refresh()
        /* A draft on screen is a draft the player may change their mind about. */
        if let staged, Uttt.messageText == staged.text { model.setPending(true) }
        live = model

        /* The model does not know there is a conversation and should not. It
         * says the position changed; a position this device can no longer
         * move in is a move this device just made. */
        model.$positionKey
            .dropFirst()
            .sink { [weak self] _ in
                guard let self else { return }
                /* A REPLACEMENT RESTAGES, it does not stage a second bubble:
                 * the undo half of a change of mind hands the move back, and
                 * the move that replaces it arrives a beat later. */
                guard !Uttt.canMove, let wire = UtttWire.resident, wire != self.staged else { return }
                self.stage(wire, in: conversation)
                /* THE END OF THE GAME re-presents, for the door the playing
                 * screen did not have: Again. */
                if Uttt.over != .none {
                    DispatchQueue.main.async { self.present(conversation) }
                }
            }
            .store(in: &bag)

        show(UtttGameScreen(model: model, door: door) { [weak self] in
            self?.again(in: conversation)
        })
    }

#if DEBUG
    /// A game `plies` moves in, both seats taken, seated as `dev.seat` says:
    /// "a" is the creator (O), "b" the joiner (X).
    private func showSeeded(_ plies: Int, _ conversation: MSConversation) {
        let seed = UtttDev.seed

        /* WHERE THE GAME ACTUALLY IS, if anybody has moved. */
        if let live = UtttDev.live, Uttt.read(live), Uttt.seed == seed {
            showBoard(mark: Uttt.myMark, door: Uttt.door(sent: true), conversation)
            return
        }

        /* A CONSTANT, NOT A SEARCH: the first moves of the game the render
         * harness draws, so a seeded screenshot and a PPM are the same board. */
        Uttt.newGame(seed: seed)
        let opening = [34, 67, 44, 80, 76, 43, 69, 62, 79, 63, 4, 40, 39, 31,
                       37, 16, 70, 71, 72, 3, 29, 19, 17, 73, 14, 50, 45, 6]
        for mv in opening.prefix(max(0, plies)) where Uttt.over == .none {
            _ = Uttt.play(mv)
        }
        Uttt.seat(o: UtttDev.identity("a"), x: UtttDev.identity("b"))
        showBoard(mark: Uttt.myMark, door: Uttt.door(sent: true), conversation)
    }
#endif

    private func show<V: View>(_ screen: V) {
        UtttLog.note("show", String(String(describing: V.self).prefix(40)))
        guard appeared else {
            pendingScreen = AnyView(screen)
            return
        }
        attach(AnyView(screen))
    }

    private func attach(_ screen: AnyView) {
        host?.willMove(toParent: nil)
        host?.view.removeFromSuperview()
        host?.removeFromParent()

        let vc = UIHostingController(rootView: screen)
        addChild(vc)
        vc.view.frame = view.bounds
        vc.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        /* PAPER, NOT CLEAR, for the frame between a swap and the first layout
         * of the new screen - a clear host is the dark drawer showing through. */
        vc.view.backgroundColor = UtttPaper.flat
        view.addSubview(vc.view)
        vc.didMove(toParent: self)
        host = vc
    }
}
