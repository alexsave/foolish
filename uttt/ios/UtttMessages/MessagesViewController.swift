import Messages
import UIKit
import UtttKit

/// The extension. It owns the conversation and nothing else - every rule and
/// every coordinate is the kernel's, the screens are UtttKit's, and what a
/// bubble carries, who sits where, what they may do and which door they get
/// is `uttt_msg.h`'s.
///
/// NO UNDO AND NO TAKE-BACK (owner, 2026-09-22, over UI.html 02): the only
/// way to change a move is to tap another square, which replaces the staged
/// draft. Messages' own X on the draft is system UI and is honoured - the
/// board reverts - but nothing here offers a way back of its own.
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

    /// The screen up, a UIKit view (TESTFLIGHT_PLAN 14: no SwiftUI in this
    /// process).
    private var host: UIView?

    /// THE AUTO-COLLAPSE, on the render server (CollapseSlide): armed right
    /// before this controller asks for compact, and every screen rides it.
    private let slide = CollapseSlide.uttt()

    /// THE SEND HINT AND THE SEND DOOR, over whatever screen is up (see
    /// UtttSendOverlay for why over and not in), above every screen `attach`
    /// puts in, and never swapped.
    private lazy var overlay = UtttSendOverlay { [weak self] in self?.sendDoorTapped() }

    /// The insert the send door re-issues: the bubble, and the stage it
    /// belongs to - a newer stage or a cancel makes it void.
    private var doorInsert: (message: MSMessage, generation: Int, conversation: MSConversation)?

    /// The stage whose insert Messages answered with a yes. Every watchdog of
    /// that stage stands down at once, including one armed by an earlier try
    /// whose answer arrived late.
    private var landedGeneration = -1

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

    /// THIS DEVICE'S NEWEST IS A NEW GAME, so it beats the selection even
    /// though the kernel ranks a different game's tapped bubble first. Set by
    /// Again, whose finished game stays the selection through the draft AND
    /// the send; cleared by the next tap or activation.
    private var draftIsNewGame = false

    /// The draft currently in the input field. Messages reports a REPLACED
    /// bubble as cancelled, so a cancel that does not name this one is stale.
    private var draftURL: URL?

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
#if DEBUG
        if UtttDev.empty { UtttLog.note("load", "dev.empty - nothing"); return }
#endif
        UtttLog.note("load", "speaks \(UtttLanguage.apply())")
        /* CLEAR UNTIL IT APPEARS - see `appeared`. */
        view.backgroundColor = .clear

        overlay.frame = view.bounds
        overlay.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        /* NO FLUSH HERE: onGrow runs inside the layout pass that lays the
         * sheet out at the taller height. A flush there committed the half-
         * done pass - the view already taller, the sheet still at compact -
         * so every expand from compact showed one frame of the board riding
         * the drawer's top before it re-laid out (TESTFLIGHT_PLAN 18). The
         * pass's own commit takes the hint down in the same frame. */
        overlay.onGrow = { [weak self] in self?.hideHintNow(flush: false) }
        view.addSubview(overlay)
#if DEBUG
        devWatchForArrivals()
#endif
    }

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
#if DEBUG
        if UtttDev.empty { return }
#endif
        UtttLanguage.apply()
        UtttLog.note("active", "\(styleName), selected \(conversation.selectedMessage != nil)")
        becameActiveAt = Date()
        overlay.compact = presentationStyle == .compact
        arrived = nil
        draftIsNewGame = false
        unbound = conversation.selectedMessage == nil
#if DEBUG
        seatChosen = false
#endif
        present(conversation, motion: .open)
        /* A DEADLINE ON THE WAIT BELOW, so a host that never sends one of the
         * two signals cannot leave the drawer blank or the invitation unstaged
         * - it is logged, and everything waiting runs anyway. */
        let activation = becameActiveAt
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            guard let self, self.becameActiveAt == activation, !self.ready else { return }
            UtttLog.fault("ready", "no \(self.appeared ? "" : "viewDidAppear ")\(self.conversationActive ? "" : "didBecomeActive")after 3s; going ahead")
            self.appeared = true
            self.conversationActive = true
            self.becameReady()
        }
    }

    /// THE CONVERSATION IS LIVE. The second of the two things an insert
    /// waits for (see `ready`).
    override func didBecomeActive(with conversation: MSConversation) {
        super.didBecomeActive(with: conversation)
        UtttLog.note("did-active")
        conversationActive = true
        becameReady()
    }

    override func willResignActive(with conversation: MSConversation) {
        super.willResignActive(with: conversation)
        UtttSeats.flush()
        conversationActive = false
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
    /// in and paper is painted under it.
    ///
    /// AND NOTHING IS INSERTED UNTIL THE DRAWER IS UP AND THE CONVERSATION IS
    /// ACTIVE. The first TestFlight build inserted the invitation from inside
    /// willBecomeActive - before didBecomeActive, before the view was in a
    /// window, with `activeConversation` still nil - and on a real phone the
    /// bubble never reached the input field (on the simulator it did). foolish
    /// never inserts that early: its create runs from a tap on a drawer that
    /// is already up, and its stage takes `activeConversation`. So an insert
    /// here waits for both viewDidAppear and didBecomeActive (`ready`), with a
    /// logged deadline in case either never comes.
    private var appeared = false
    private var conversationActive = false
    private var ready: Bool { appeared && conversationActive }
    private var pendingScreen: UIView?
    private var afterReady: [() -> Void] = []

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UtttLog.note("appear", "\(Int(view.bounds.width))x\(Int(view.bounds.height)) \(styleName)")
        /* ON A PHONE THE FIRST viewDidAppear IS AT THE WHOLE WINDOW (430x932),
         * a second before the drawer is up (430x343), and an insert issued in
         * between is dropped by Messages with no completion at all - device
         * log 2026-09-23. Only a drawer counts as up, and which appearance is
         * one is shared (ms_drawer_up, InsertStaging): never the window itself, an
         * expanded drawer at any height short of it (the SE's 647 of 667). */
        if drawerUp {
            appeared = true
        } else {
            UtttLog.note("appear", "window-sized (\(styleName)); not up yet")
        }
        view.backgroundColor = UtttPaper.flat
        if let screen = pendingScreen {
            pendingScreen = nil
            attach(screen)
        }
        becameReady()
    }

    /// THE DRAWER HAS ITS SIZE a beat before it appears: the first layout at
    /// less than the whole window is the compact (or expanded) drawer, and
    /// painting then saves the one black frame between Messages' grey card
    /// and the paper that waiting for viewDidAppear left.
    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        guard !appeared, !sized, drawerUp else { return }
        UtttLog.note("sized", "\(Int(view.bounds.width))x\(Int(view.bounds.height))")
        sized = true
        becameReady()
    }
    private var sized = false

    /// The view is a drawer, not the window it was first laid out at.
    private var drawerUp: Bool {
        guard let window = view.window else { return false }
        return InsertStaging.drawerUp(window: window.bounds.height, view: view.bounds.height,
                                      expanded: presentationStyle == .expanded)
    }

    private func becameReady() {
        if appeared || sized, let screen = pendingScreen {
            pendingScreen = nil
            view.backgroundColor = UtttPaper.flat
            attach(screen)
        }
        guard ready else { return }
        let work = afterReady
        afterReady.removeAll()
        work.forEach { $0() }
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        appeared = false
        sized = false
    }

    /// Run `work` once the drawer is up and the conversation is active.
    private func whenReady(_ work: @escaping () -> Void) {
        if ready { work() } else { afterReady.append(work) }
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
        present(conversation, motion: .open)
    }

    /// A move from the other player, which does NOT become the selection.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        super.didReceive(message, conversation: conversation)
        /* MY OWN BUBBLE COMING BACK IS NOT AN ARRIVAL (foolish's round 12
         * #11, `StagedBubbleRouting.isMine`). A drawer opened by tapping a
         * bubble is bound to that bubble's session, and Messages hands the
         * sender's own bubble back through here the moment the arrow is
         * pressed - a second before didStartSending on the simulator (log
         * 2026-09-23: `receive` at +198.648s, `send` at +199.684s), and to a
         * second device on the same account for real. Threaded on as an
         * arrival it rebuilt the screen and played the whole move again, and
         * then the Send's own post-settlement played on top: the owner's
         * "my own move replays after Send". The board already holds these
         * exact bytes, so nothing is folded in - but this IS the send landing,
         * so the post-settlement plays now rather than a second late. */
        if let wire = UtttWire(url: message.url) {
            switch InsertStaging.receive(mine: isMine(wire), staged: wire == staged) {
            case .arrival: break
            case .echo:
                UtttLog.note("receive-dropped", "my own bubble")
                return
            case .echoOfStaged:
                UtttLog.note("receive-dropped", "my own bubble")
                settleSent(wire, conversation)
                return
            }
        }
        UtttLog.note("receive")
        arrived = UtttWire(url: message.url)
        present(conversation, motion: .arrival)
    }

#if DEBUG
    /// Polls `dev.arrive` (UtttDev.takeArrival) every 0.4s for as long as the
    /// extension lives. foolish's RIG_ARRIVE, for the same reason: one
    /// simulator cannot send this drawer a move, so the rig says one arrived.
    private var devArriveTimer: Timer?

    private func devWatchForArrivals() {
        guard devArriveTimer == nil else { return }
        devArriveTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { [weak self] _ in
            guard let self, let arg = UtttDev.takeArrival() else { return }
            self.devArrive(arg)
        }
    }

    /// The other dev seat plays into the game on screen, and the result goes
    /// through exactly the lines `didReceive` runs - so the board shows what a
    /// second phone's bubble would have shown: channel E.
    private func devArrive(_ arg: String) {
        guard let conversation = activeConversation,
              let mine = UtttDev.seat,
              let showing = Uttt.messageText else {
            UtttLog.fault("dev", "arrive: needs an open drawer, dev.seat and a game on screen")
            return
        }
        let other = mine == "a" ? "b" : "a"
        UtttSeats.use(other)                   /* the other phone's records */
        Uttt.me(UtttDev.rotate(UtttDev.identity(other)))
        Uttt.sender(of: nil)
        defer { identify(conversation) }
        guard Uttt.read(showing), Uttt.canMove else {
            UtttLog.fault("dev", "arrive: it is not \(other)'s move")
            return
        }
        let legal = Uttt.legal
        let mv = Int(arg) ?? (legal.isEmpty ? -1 : Int(legal[legal.count / 2]))
        guard Uttt.playAsMe(mv), let text = Uttt.messageText else {
            UtttLog.fault("dev", "arrive: \(other) cannot play \(mv)")
            _ = Uttt.read(showing)
            return
        }
        UtttLog.note("dev", "arrive: \(other) plays \(mv)")
        if UtttDev.game != nil { UtttDev.live = text }
        /* didReceive, line for line. */
        UtttLog.note("receive")
        arrived = UtttWire(text: text)
        present(conversation, motion: .arrival)
    }
#endif

    /// THE HUMAN TAPPED THE ARROW: the draft is in the thread now.
    ///
    /// The message Messages hands over is the authority - the bytes that
    /// actually went out - not our own bookkeeping, which a replaced draft or
    /// a torn-down extension can have lost.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        super.didStartSending(message, conversation: conversation)
        /* A SENT STAGE IS FINISHED: no watchdog, retry, paint/collapse wait or
         * send door may re-insert it. Each of them checks the generation, so
         * bumping it here stands them all down, exactly as a cancel does; a
         * late-answered insert otherwise put the sent move back in the field. */
        stageGeneration += 1
        UtttSeats.flush()                      /* a join records X as it is played */
        let wire = UtttWire(url: message.url) ?? staged
        UtttLog.note("send", wire.map { "\($0.text.count) chars" } ?? "NO PAYLOAD")
        markSent(wire)
        if message.url == draftURL {
            draftURL = nil
            staged = nil
        }
        live?.setPending(false)
        hideHintNow()
        overlay.staged = false
        overlay.door = false
        doorInsert = nil
        let wasUnbound = unbound
        if let wire { settleSent(wire, conversation) } else { present(conversation, motion: .settle) }

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

    /// The bubble whose post-settlement has played. The echo and
    /// didStartSending both mean "this bubble went", in either order, and
    /// the highlighter moves once.
    private var settled: UtttWire?

    /// B: THE POST-SETTLEMENT PLAYS AT SEND - the highlighter goes to the
    /// outlined block and nothing else moves. On the board already up when it
    /// is this game (the usual case); a fresh present only when it is not.
    /// Once per bubble, whichever of the two send signals comes first.
    private func settleSent(_ wire: UtttWire, _ conversation: MSConversation) {
        guard settled != wire else {
            UtttLog.note("settle", "already played for this bubble")
            return
        }
        settled = wire
        hideHintNow()
        if let live, wire.load(), live.seed == Uttt.seed, Uttt.messageText == wire.text {
            live.sent()
        } else {
            present(conversation, motion: .settle)
        }
    }

    /// A bubble this device staged or sent - the draft in the field, or the
    /// last one that went.
    private func isMine(_ wire: UtttWire) -> Bool {
        wire.url == draftURL || wire == staged || wire == sent
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
        overlay.staged = false
        overlay.door = false
        doorInsert = nil

        /* THE X IS THE UNDO - the only one there is, by the owner's decision
         * (no undo button, no take-back door): Messages' own X on the draft
         * cannot be removed, so the board must follow it. What comes back is
         * the kernel's rule: only my own move, and taking back a joining move
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
        /* THE HINT GOES AS THE DRAWER STARTS TO GROW, not once it has: the
         * Send button is only above a compact drawer. */
        if presentationStyle != .compact { hideHintNow() }
        overlay.compact = presentationStyle == .compact
    }

    /// THE HINT DOWN IN THIS FRAME. A send, or a drawer that has started to
    /// grow: a fade would take it down over its duration, and a send is the
    /// moment this process is busiest (the re-present) - filmed lingering
    /// 0.8-2s after the arrow and ~1s into a drag. The overlay's layer is
    /// hidden and committed now; it comes back when the drawer is compact
    /// and a bubble is staged again (`showHintLayer`).
    private func showHintLayer() {
        overlay.hintLayerShown = true
        overlay.rest = overlay.bounds.height
        overlay.compact = true
        overlay.layer.opacity = 1
    }

    /// `flush` pushes it to the render server now - right outside a layout
    /// pass (a send, a style change), never inside one (see `onGrow`).
    private func hideHintNow(flush: Bool = true) {
        guard overlay.hintLayerShown else { return }
        overlay.hintLayerShown = false
        overlay.compact = false
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        overlay.layer.opacity = 0
        CATransaction.commit()
        if flush { CATransaction.flush() }
    }

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
        UtttLog.note("style", Self.name(presentationStyle))
        overlay.compact = presentationStyle == .compact
        if presentationStyle == .compact { showHintLayer() }
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

    private func present(_ conversation: MSConversation, motion: Uttt.Channel = .still) {
        live?.onPosition = nil

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
                DispatchQueue.main.async { self.present(conversation, motion: motion) }
            })
            return
        }

        /* STRAIGHT TO THE BOARD with `dev.game` set: see UtttDev. Seated
         * first - the seeded board's "you are" is a seat question too. */
        identify(conversation)
        if conversation.selectedMessage == nil, staged == nil, sent == nil,
           reverted == nil, let plies = UtttDev.game {
            showSeeded(plies, motion, conversation)
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
            show(UtttLobbyScreen(stance: .unreadable, slide: slide))
            return
        }

        /* MY OWN MOVE DOES NOT REPLAY AT ME (owner, 2026-09-23; foolish's
         * quiet open, didStartSending + lastSentPayload): the bubble this
         * device just sent, shown again - Messages re-presenting it, or the
         * human tapping it - is the settled board. The board already showed
         * that move being made; only the highlighter's post-settlement
         * played, at Send. */
        var motion = motion
        if motion == .open, let mine = sent, mine.text == wire.text {
            UtttLog.note("present", "my own bubble, just sent - quiet")
            motion = .still
        }
        /* The witness first: asking for the seat records it, and then every
         * later answer is "by record". */
        UtttLog.note("present", "seed \(Uttt.seed) by \(Uttt.seatBy) seat \(Uttt.seat) plies \(Uttt.plyCount) door \(Uttt.door)")
        showSeat(motion, conversation)
        UtttSeats.flush()
#if DEBUG
        /* `dev.restage` (store frames only): put the tapped board, unchanged,
         * back into the field in this session - see UtttDev.takeRestage. */
        if UtttDev.takeRestage(), let same = UtttWire.resident {
            UtttLog.note("present", "dev.restage: the tapped board, unchanged")
            whenReady { [weak self] in
                DispatchQueue.main.async { self?.stage(same, in: conversation) }
            }
        }
#endif
    }

    /// The screen for the resident game, by this device's seat. One owner,
    /// so the DEBUG seeded path cannot show a spectator a player's screen.
    private func showSeat(_ motion: Uttt.Channel, _ conversation: MSConversation) {
        let door = Uttt.door
        /* WHICH SEAT IS THIS DEVICE'S is the kernel's answer: its record of
         * the game, else the tag of this device's participant, else - in a
         * DM - who sent the bubble (utm_resolve). */
        switch Uttt.seat {
        case .waiting:
            /* No door here: see UtttLobbyScreen. */
            show(UtttLobbyScreen(stance: .waiting, slide: slide))

        case .open, .x, .o:
            /* OPENING SOMEBODY'S INVITATION IS SITTING DOWN AS X, and the
             * first move is yours: the join and the first move are one
             * message, staged when the move is made. */
            showBoard(mark: Uttt.myMark, door: door, motion: motion, conversation)

        case .spectator:
            let model = UtttModel(seed: Uttt.seed, you: .none)
            model.refresh()
            show(UtttWatchScreen(model: model, door: door, slide: slide,
                                 onDoor: { [weak self] in self?.again(in: conversation) },
                                 onRules: { [weak self] in self?.openRules() },
                                 onDiagnostics: { [weak self] in self?.openDiagnostics(conversation) }))
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

    /// WHO THIS DEVICE IS, told to the kernel before every question about a
    /// seat: its identity, its seat records, and who sent the tapped bubble.
    ///
    /// THE IDENTITY IS NOT STABLE. Messages' participant id is a random UUID
    /// it deletes with the extension (a reinstall, a TestFlight <->
    /// development swap), so the kernel asks this device's own record of the
    /// game first and the tag second (UtttSeats, utm_resolve).
    private func identify(_ conversation: MSConversation) {
#if DEBUG
        if let word = UtttDev.seat {
            UtttSeats.use(word)
            Uttt.me(UtttDev.rotate(UtttDev.identity(word)))
            /* Both dev seats are this one participant, so who sent a bubble
             * says nothing about which of them is holding it. */
            Uttt.sender(of: nil)
            return
        }
#endif
        UtttSeats.use("")
        var id = withUnsafeBytes(of: conversation.localParticipantIdentifier.uuid) { Data($0) }
#if DEBUG
        id = UtttDev.rotate(id)
#endif
        Uttt.me(id)
        tellSender(conversation)
    }

    /// THE SENDER WITNESS: did this device send the tapped bubble, in a chat
    /// with exactly one other person. Both ids come from Messages' table at
    /// this moment, so the comparison holds after the ids rotate; it is made
    /// here, live, and never stored. The kernel applies it only while that
    /// exact message is the one on the board.
    private func tellSender(_ conversation: MSConversation) {
        guard let sel = conversation.selectedMessage, let text = sel.url?.absoluteString else {
            Uttt.sender(of: nil)
            return
        }
        Uttt.sender(of: text,
                    isDM: conversation.remoteParticipantIdentifiers.count == 1,
                    iSent: sel.senderParticipantIdentifier == conversation.localParticipantIdentifier)
    }

    /// This device's newest against what Messages handed over. The kernel
    /// decides, including which of two joiners got the seat; a different game
    /// tapped wins, unless the draft is a new game this device just asked for.
    private func current(_ tapped: UtttWire?) -> UtttWire? {
        if let r = reverted { reverted = nil; return r }
        /* Staged or already sent: after Again's invitation goes out, the
         * finished game is still the selection, and "a different game tapped
         * wins" would put the old board back up under the new invitation. */
        if draftIsNewGame, let mine = staged ?? sent { return mine }
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
#if DEBUG
        /* `dev.invite` (store frames only): see UtttDev.takeSeededInvite. */
        if UtttDev.takeSeededInvite() {
            Uttt.openInvitation(at: Date(timeIntervalSince1970: TimeInterval(UtttDev.seed)))
        } else {
            Uttt.openInvitation()
        }
#else
        Uttt.openInvitation()
#endif
        guard let wire = UtttWire.resident else {
            UtttLog.fault("start", "the kernel wrote no invitation")
            return
        }
        UtttLog.note("start")
        staged = wire
        present(conversation)
        whenReady { [weak self] in
            DispatchQueue.main.async {
                guard let self, self.staged == wire else { return }
                self.stage(wire, in: conversation)
            }
        }
    }

    /// AGAIN (docs/UI.html 06, 07): a fresh invitation from whoever asks, in
    /// the FINISHED GAME'S session (sessionFor). The open drawer is bound to
    /// that session and the host delivers didReceive only for it, so a new
    /// session left the reply unheard; the cost is that Messages collapses
    /// the finished game's last bubble to its caption.
    /// Whoever proposes moves second - the lobby rule - which is also what
    /// swaps the players for a rematch.
    private func again(in conversation: MSConversation) {
        UtttLog.note("again")
        identify(conversation)
        draftIsNewGame = true
        sent = nil
        arrived = nil
        start(in: conversation)
    }

    // MARK: staging

    /// ONE MSSession PER GAME. Messages collapses every older bubble of a
    /// session down to its caption and keeps only the newest interactive, and
    /// a message in a session is one Messages will hand back when tapped.
    /// The one exception is Again: its new game stays in the tapped finished
    /// game's session, because an open drawer hears only its own session.
    private var session: MSSession?
    private var sessionGame: UtttWire?

    private func sessionFor(_ wire: UtttWire, _ conversation: MSConversation) -> MSSession {
        if let s = session, let g = sessionGame, g.isSameGame(as: wire) { return s }
        let selected = conversation.selectedMessage
        let s: MSSession
        if let sel = selected?.session,
           draftIsNewGame || UtttWire(url: selected?.url)?.isSameGame(as: wire) == true {
            s = sel        // Again stays in the finished game's session, so replies reach this drawer
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
        /* A NEW STAGE STARTS THE HINT'S WAIT AGAIN, and takes down a door left
         * by the stage it replaces. The hint comes back once this one lands. */
        overlay.staged = false
        overlay.door = false
        doorInsert = nil

        /* BAKED NOW, from the message being staged, before anything can load
         * a different one into the kernel's one resident slot. */
        guard wire.load() else {
            UtttLog.fault("stage", "the kernel cannot read what it wrote")
            return
        }
        let message = MSMessage(session: sessionFor(wire, conversation))
        message.url = wire.url
        /* THE PICTURE IS PAINTED OFF THE MAIN THREAD. Everything it needs is
         * read from the kernel here, in a millisecond; the fourteen thousand
         * fills at 3x took a fifth of a second on the main thread at every
         * stage, which is exactly when the board's highlighter is travelling
         * (docs/UI.html: the drawer and the bubble move once the ink lands). */
#if DEBUG
        UtttLog.mem("stage")
#endif
        let snap = UtttBubble.snapshot(display: traitCollection.displayScale)
#if DEBUG
        UtttLog.mem("snapshot")
#endif
        let caption = UtttBubble.caption
        /* THE COLLAPSED LINE IS OURS TOO, or Messages writes "<phone number>
         * sent Ultimate message" into a thread about a board. */
        message.summaryText = caption
#if DEBUG
        if let forced = UtttDev.takeCaption(), !forced.isEmpty {
            UtttLog.note("stage", "dev.caption: summary '\(forced)' for '\(caption)'")
            message.summaryText = forced
        }
#endif

        staged = wire
        draftURL = message.url
        live?.setPending(true)
#if DEBUG
        if UtttDev.game != nil { UtttDev.live = wire.text }
#endif

        /* Painted off the main thread, and handed over only once the board
         * has settled - see UtttMotionClock.whenSettled. */
        let painted: (@escaping (UIImage) -> Void) -> Void = { [weak self] done in
            DispatchQueue.global(qos: .userInitiated).async {
                let img = UtttBubble.image(snap)
                DispatchQueue.main.async {
#if DEBUG
                    UtttLog.mem("painted")
#endif
                    if let clock = self?.live?.clock { clock.whenSettled { done(img) } }
                    else { done(img) }
                }
            }
        }
        if presentationStyle == .compact {
            painted { [weak self] img in
                guard let self, self.stageGeneration == generation else { return }
#if DEBUG
                UtttLog.mem("settled")
#endif
                message.layout = UtttBubble.layout(image: img, caption: caption)
#if DEBUG
                UtttLog.mem("layout")
#endif
                self.insert(message, generation: generation, in: conversation, attempt: 1)
            }
            return
        }
        UtttLog.note("stage", "collapsing after the move and the rest")
        /* THE PAINT AND THE COLLAPSE RUN TOGETHER, and the transition is
         * waited for from NOW: waiting for it after the paint missed a
         * collapse that had already finished and sat out the whole timeout. */
        var image: UIImage?
        var imageWaiter: CheckedContinuation<UIImage, Never>?
        painted { img in
            if let w = imageWaiter { imageWaiter = nil; w.resume(returning: img) } else { image = img }
        }
        Task { @MainActor [weak self] in
            await self?.restThenCollapse(generation)
            let img: UIImage
            if let ready = image { img = ready } else {
                img = await withCheckedContinuation { imageWaiter = $0 }
            }
            message.layout = UtttBubble.layout(image: img, caption: caption)
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
    ///
    /// AND A SILENT ONE IS A REFUSAL TOO. ChatKit drops an insert that arrives
    /// before the host counts the drawer as presenting and never calls back
    /// (shared/c/msg_stage/INSERT_GATING.md), so every try arms a watchdog, and
    /// what its silence means is shared (ms_insert_silence): in the compact
    /// drawer, try again every half second up to ten times; expanded, where
    /// the host parks an accepted insert's answer on purpose, keep listening
    /// and count nothing; out of tries, hand the human the send door.
    private func insert(_ message: MSMessage, generation: Int,
                        in conversation: MSConversation, attempt: Int) {
        /* THE ACTIVE CONVERSATION when there is one - foolish's stage uses
         * nothing else - and the one we were handed only as a fallback. */
        let target = activeConversation ?? conversation
        UtttLog.note("insert", "attempt \(attempt)\(activeConversation == nil ? " (no active conversation)" : "")")
        overlay.door = false
        var answered = false
        watchSilence(of: message, generation: generation, in: conversation,
                     attempt: attempt) { answered }
        let answer: (Error?) -> Void = { [weak self] error in
            DispatchQueue.main.async {
                if answered { UtttLog.note("insert", "late answer for attempt \(attempt)") }
                answered = true
                guard let self else { return }
                guard let error else {
                    UtttLog.note("inserted")
#if DEBUG
                    UtttLog.mem("inserted")
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { UtttLog.mem("inserted+1s") }
#endif
                    guard self.stageGeneration == generation else { return }
                    /* IN THE FIELD: every watchdog of this stage stands down,
                     * and the hint's wait starts now. */
                    self.landedGeneration = generation
                    self.doorInsert = nil
                    self.overlay.door = false
                    self.overlay.staged = true
                    self.overlay.restart()
                    if self.presentationStyle == .compact { self.showHintLayer() }
                    return
                }
                UtttLog.fault("insert", "attempt \(attempt) failed: \(error.localizedDescription)")
                guard self.stageGeneration == generation else { return }
                if attempt < 3 {
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
#if DEBUG
        /* `dev.dropinsert`: swallowed with no answer, as ChatKit's gate does. */
        if UtttDev.dropInsert {
            UtttLog.note("insert", "dev.dropinsert - swallowed")
            return
        }
#endif
#if DEBUG
        UtttLog.mem("insert")
#endif
        target.insert(message, completionHandler: answer)
    }

    /// One try's watchdog: after the kernel's silence, if nobody answered and
    /// the stage is still current and not landed, ask the kernel what the
    /// silence means. Every firing is logged, so a device log shows how many
    /// tries the host needed.
    private func watchSilence(of message: MSMessage, generation: Int,
                              in conversation: MSConversation, attempt: Int,
                              answered: @escaping () -> Bool) {
        DispatchQueue.main.asyncAfter(deadline: .now() + InsertStaging.silenceSeconds) { [weak self] in
            guard let self, !answered(), self.stageGeneration == generation,
                  self.landedGeneration != generation else { return }
            let compact = self.presentationStyle == .compact
            switch InsertStaging.silence(attempt: attempt, compact: compact) {
            case .listen:
                UtttLog.note("insert", "attempt \(attempt) unanswered while \(self.styleName); listening")
                self.watchSilence(of: message, generation: generation, in: conversation,
                                  attempt: attempt, answered: answered)
            case .retry:
                UtttLog.fault("insert", "attempt \(attempt) got no answer; retrying")
                self.insert(message, generation: generation, in: conversation, attempt: attempt + 1)
            case .door:
                UtttLog.fault("insert", "attempt \(attempt) got no answer; \(attempt) unanswered, offering the send door")
                self.doorInsert = (message, generation, conversation)
                self.overlay.door = true
                if compact { self.showHintLayer() }
            }
        }
    }

    /// THE SEND DOOR, tapped: the same bubble, inserted again from a drawer
    /// that is by now certainly presenting. Its tries count from one.
    private func sendDoorTapped() {
        guard let d = doorInsert, d.generation == stageGeneration else {
            UtttLog.note("door", "send tapped for a stage that is gone")
            overlay.door = false
            return
        }
        UtttLog.note("door", "send tapped")
        doorInsert = nil
        insert(d.message, generation: d.generation, in: d.conversation, attempt: 1)
    }

    /// THE DRAWER MOVES ONCE THE MOVE HAS SETTLED AND RESTED (UI.html: once
    /// the ink lands, never during; owner: "let it breathe"). The whole plan
    /// runs - ink, highlighter - then the kernel's rest with nothing
    /// moving, and only then the slide is armed and compact asked for;
    /// foolish's `stage` waits for its board to settle and rests 500 ms the
    /// same way. The bubble goes in once the transition has run.
    private func restThenCollapse(_ generation: Int) async {
        if let clock = live?.clock {
            await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in
                clock.whenDone { k.resume() }
            }
        }
        try? await Task.sleep(nanoseconds: UInt64(Uttt.restSeconds * 1_000_000_000))
        guard stageGeneration == generation, presentationStyle != .compact else { return }
        UtttLog.note("stage", "collapsing")
        slide.arm()
        requestPresentationStyle(.compact)
        await awaitTransitionSettled()
        /* didTransition comes BEFORE the compact height is handed (measured:
         * 50 ms before), so the arm outlives it by a beat; an arm no height
         * ever answered stands down then. */
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.slide.disarm()
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
    private func showBoard(mark: Uttt.Mark, door: Uttt.Door, motion: Uttt.Channel = .still,
                           _ conversation: MSConversation) {
        let model = UtttModel(seed: Uttt.seed, you: mark)
        /* A draft on screen is a draft the player may change their mind about. */
        let draft = staged.map { Uttt.messageText == $0.text } ?? false
        if draft { model.setPending(true) }
        /* THE LAST MOVE ARRIVES through the door it came in by (docs/UI.html
         * "How it moves"): an opened bubble replays it, an arrival draws it
         * in, and a send or a cancel shows the board at rest - a draft's at
         * rest with its settlement held for Send (UI.html channel B). */
        model.show(draft && motion == .still ? .draft : motion)
        live = model

        /* The model does not know there is a conversation and should not. It
         * says the position changed; a position this device can no longer
         * move in is a move this device just made. */
        model.onPosition = { [weak self] in
            guard let self else { return }
            /* A REPLACEMENT RESTAGES, it does not stage a second bubble:
             * the undo half of a change of mind hands the move back, and
             * the move that replaces it arrives a beat later. */
            guard !Uttt.canMove, let wire = UtttWire.resident, wire != self.staged else { return }
            self.stage(wire, in: conversation)
            /* THE END OF THE GAME re-presents, for the door the playing
             * screen did not have: Again. */
            if Uttt.over != .none {
                /* Not in the middle of the slide: a new screen there
                 * lands on a host with no push. */
                DispatchQueue.main.async {
                    self.slide.whenStill { self.present(conversation) }
                }
            }
        }

        show(UtttGameScreen(model: model, door: door, slide: slide,
                            onDoor: { [weak self] in self?.again(in: conversation) },
                            onRules: { [weak self] in self?.openRules() },
                            onDiagnostics: { [weak self] in self?.openDiagnostics(conversation) }))
    }

#if DEBUG
    /// A game `plies` moves in, both seats taken, seated as `dev.seat` says:
    /// "a" is the creator (O), "b" the joiner (X).
    private func showSeeded(_ plies: Int, _ motion: Uttt.Channel, _ conversation: MSConversation) {
        let seed = UtttDev.seed

        /* WHERE THE GAME ACTUALLY IS, if anybody has moved. */
        if let live = UtttDev.live, Uttt.read(live), Uttt.seed == seed {
            showSeat(motion, conversation)
            return
        }

        /* A CONSTANT, NOT A SEARCH: the first moves of the game the render
         * harness draws, so a seeded screenshot and a PPM are the same board. */
        Uttt.newGame(seed: seed)
        let opening = [34, 67, 44, 80, 76, 43, 69, 62, 79, 63, 4, 40, 39, 31,
                       37, 16, 70, 71, 72, 3, 29, 19, 17, 73, 14, 50, 45, 6]
        for mv in UtttDev.moves ?? Array(opening.prefix(max(0, plies))) where Uttt.over == .none {
            _ = Uttt.play(mv)
        }
        Uttt.seat(o: UtttDev.identity("a"), x: UtttDev.identity("b"))
        showSeat(motion, conversation)
    }
#endif

    private func show(_ screen: UIView) {
        UtttLog.note("show", String(String(describing: type(of: screen)).prefix(40)))
        guard appeared || sized else {
            pendingScreen = screen
            return
        }
        attach(screen)
    }

    private func attach(_ screen: UIView) {
        host?.removeFromSuperview()
        slide.end()
        screen.frame = view.bounds
        screen.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        /* UNDER THE SEND OVERLAY, which stays on top of every screen, and
         * laid out before it is seen: the first frame has the lines. */
        view.insertSubview(screen, belowSubview: overlay)
        screen.layoutIfNeeded()
        host = screen
        slide.host = screen
    }

    /// THE RULES, a sheet of their own over the drawer: a swipe down closes
    /// the rules and leaves the game up.
    private func openRules() {
        guard presentedViewController == nil else { return }
        present(UtttRulesSheet(), animated: true)
    }

    // MARK: diagnostics (hold the rulebook) - 1.0(9)

    /// Every input to the seat verdict, and the TEMPORARY claim.
    private func openDiagnostics(_ conversation: MSConversation) {
        guard presentedViewController == nil else { return }
        identify(conversation)
        let seed = Uttt.seed
        /* The resident message when the panel opened: the claim is about
         * that game, whatever is resident by the time a button is tapped. */
        let game = Uttt.messageText
        let sheet = UtttDiagnosticsSheet(
            text: { [weak self] in self?.diagnostics(conversation) ?? "" },
            canClaimX: Uttt.sealed, hasClaim: Uttt.record != nil
        ) { [weak self] action in
            guard let self else { return }
            self.identify(conversation)
            if let game { Uttt.read(game) }
            switch action {
            case .claimO:     Uttt.claim(.o)
            case .claimX:     Uttt.claim(.x)
            case .clearClaim: Uttt.forgetSeat()
            }
            UtttSeats.flush()
            UtttLog.note("claim", "\(action) seed \(seed)")
            self.present(conversation, motion: .still)
        }
        present(sheet, animated: true)
    }

    private func diagnostics(_ conversation: MSConversation) -> String {
        identify(conversation)
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let local = conversation.localParticipantIdentifier
        let uuidBytes = withUnsafeBytes(of: local.uuid) { Data($0) }
        let o = Uttt.tag(.o), x = Uttt.tag(.x)
        let hashed = Uttt.tag(.hashed), me = Uttt.tag(.me)
        func who(_ t: Data) -> String {
            var hits: [String] = []
            if t == hashed { hits.append("my participant") }
            if t == Uttt.tag(of: Data(local.uuidString.utf8)) { hits.append("my uuidString") }
            if t == Uttt.tag(of: Data()) { hits.append("EMPTY identity") }
            for w in ["a", "b"] where t == Uttt.tag(of: Data("dev:\(w)".utf8)) { hits.append("dev:\(w)") }
            for r in conversation.remoteParticipantIdentifiers
            where t == Uttt.tag(of: withUnsafeBytes(of: r.uuid) { Data($0) }) {
                hits.append("remote \(r.uuidString.prefix(8))")
            }
            return hits.isEmpty ? "nobody known" : hits.joined(separator: ", ")
        }
        var debug = "no", rotated = "no"
#if DEBUG
        rotated = UtttDev.rotated ? "yes (dev.rotate)" : "no"
        debug = "yes, dev.seat=\(UtttDev.seat ?? "none") dev.picker=\(UtttDev.picker)"
#endif
        let sel = conversation.selectedMessage
        var lines = [
            "app \(version) (\(build))  DEBUG \(debug)",
            "",
            "== me",
            "local participant \(local.uuidString)",
            "  bytes \(uuidBytes.hex)",
            "remote participants \(conversation.remoteParticipantIdentifiers.map(\.uuidString).joined(separator: " "))",
            "",
            "== game",
            "seed \(Uttt.seed)  plies \(Uttt.plyCount)  sealed \(Uttt.sealed)  turn \(Uttt.turn)  over \(Uttt.over)",
            "O tag \(o.hex)  = \(who(o))",
            "X tag \(x.hex)  = \(Uttt.sealed ? who(x) : "(open)")",
            "my hashed tag \(hashed.hex)",
            "my seat tag   \(me.hex)",
            "record \(Uttt.record.map { "\($0)" } ?? "none")  (this device's, for this game)",
            "seat \(Uttt.seat)  by \(Uttt.seatBy)  mark \(Uttt.myMark)  canMove \(Uttt.canMove)",
            "DM \(conversation.remoteParticipantIdentifiers.count == 1)  rotated \(rotated)",
            "why: \(Uttt.seatWhy)",
            "",
            "== messages",
            "selected sender \(sel?.senderParticipantIdentifier.uuidString ?? "none")"
                + (sel.map { $0.senderParticipantIdentifier == local ? " (me)" : "" } ?? ""),
            "selected url \(sel?.url?.absoluteString ?? "none")",
            "resident \(Uttt.messageText ?? "none")",
            "staged \(staged?.text ?? "none")",
            "sent \(sent?.text ?? "none")",
            "arrived \(arrived?.text ?? "none")",
            "draftURL \(draftURL?.absoluteString ?? "none")",
            "unbound \(unbound)  draftIsNewGame \(draftIsNewGame)  style \(styleName)",
        ]
        if let r = Uttt.replayURL { lines.append("replay \(r)") }
        return lines.joined(separator: "\n")
    }
}
