// COPIED from pickemup/ios/PickemupMessages/MessagesViewController.swift at 03eb3362 - a later lift into shared/ replaces it
//
// The extension. It owns the conversation and nothing else: every rule, every
// word and every number is the kernel's, reached ONLY through the seam
// (ChuiniuKit/Kernel/KernelSeam.swift); the screens are ChuiniuKit's.
//
// pickemup's (and uttt's) lifecycle, kept whole:
//   - NOTHING IS INSERTED UNTIL THE DRAWER IS UP AND THE CONVERSATION IS
//     ACTIVE (`whenReady`): an insert before the drawer is up is silently
//     dropped on a real phone (InsertStaging.drawerUp).
//   - ONE INSERT LOOP PER STAGE (shared/c/msg_stage via InsertStaging). A
//     stage generation voids every waiter of an older stage.
//   - MY OWN BUBBLE COMING BACK THROUGH didReceive IS NOT AN ARRIVAL.
//   - markSent NEVER REBASES BACKWARDS.
//   - ONE MSSession PER GAME.
//   - WHAT WENT OUT IS THE AUTHORITY (Kernel.sent): the staged move is
//     committed only when the sent bytes are exactly its link, and anything
//     else is adopted as sent.
//   - THE RESIDENT IS ONE SLOT, and decoding is adopting. A staged move of
//     mine is never re-read from its own staged link (`keepsStaged`), which
//     would drop it.
//   - WHO SENT THE TAPPED BUBBLE is handed to the kernel (cn_api_sender), one
//     witness of the seat resolver; the seat records and the nickname are
//     the kernel's bytes, kept by BridgeKernel.
//   - A MOVE STAGES ONCE IT HAS RESTED: the kernel's staged beats run, the
//     expanded drawer collapses, then the bubble goes in (pickemup's
//     collapse-after-settle, DECISIONS I12). A lobby bubble goes in at once.

import Messages
import ChuiniuKit
import SwiftUI
import UIKit

final class MessagesViewController: MSMessagesAppViewController {

    private lazy var host = ChuiniuHost(kernel: KernelSeam.make())
    private var hosting: UIHostingController<ChuiniuRoot>?

    private var loop = InsertStaging.Loop()
    private var stageInsert: (message: MSMessage, generation: Int, conversation: MSConversation)?
    private var stageGeneration = 0

    /// The link sitting in the input field, which nobody has sent yet.
    private var staged: URL?
    /// That bubble's own url as Messages hands it back.
    private var draftURL: URL?
    /// The last link this device SENT.
    private var sent: URL?
    /// A bubble that arrived while we were up.
    private var arrived: URL?
    /// A drawer opened from the + menu is bound to no message; its first send
    /// closes it so the next tap binds it (uttt, foolish).
    private var unbound = true

    // MARK: the view

    override func viewDidLoad() {
        super.viewDidLoad()
#if DEBUG
        ChuiniuDev.launch("viewDidLoad")
        defer { ChuiniuDev.launch("viewDidLoad done") }
        if ChuiniuDev.empty { return }
#endif
        // NEVER A BLANK DRAWER: the planks' own dark from the first commit, so
        // the drawer Messages opens while the table is still being made (the
        // conversation, the stage's first frame) is the table's colour, not
        // Messages' white; every screen paints its planks over it
        view.backgroundColor = UIColor(Ink.hold)
        KernelSeam.warm()
        watchHostBackground()
        host.onStage = { [weak self] caption, collapse in self?.stageResident(caption: caption, collapse: collapse) }
        let h = UIHostingController(rootView: ChuiniuRoot(host: host))
        h.view.backgroundColor = .clear
        h.sizingOptions = []
        addChild(h)
        h.view.frame = view.bounds
        h.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(h.view)
        h.didMove(toParent: self)
        h.view.isHidden = true              // until the drawer has its size (uttt `appeared`)
        hosting = h
    }

    private var appeared = false
    private var conversationActive = false
    private var ready: Bool { appeared && conversationActive }
    private var afterReady: [() -> Void] = []

    private var drawerUp: Bool {
        guard let window = view.window else { return false }
        return InsertStaging.drawerUp(window: window.bounds.height, view: view.bounds.height,
                                      expanded: presentationStyle == .expanded)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        if drawerUp { hosting?.view.isHidden = false }
#if DEBUG
        // THE DRAWER IS MEASURED: the hosting view fills these bounds, the
        // table screen begins the stage with the size it is given, and every
        // collapse, expand or rotation lays it out again. The rig reads this
        // line to check the two agree (chuiniu/docs/SIM_VERIFICATION.md).
        let b = view.bounds.size
        let s = KernelSeam.stage().drawer
        ChuiniuDev.log.info("drawer \(Int(b.width), privacy: .public)x\(Int(b.height), privacy: .public) stage \(s.map { "\(Int($0.width))x\(Int($0.height))" } ?? "none", privacy: .public) \(self.presentationStyle == .expanded ? "expanded" : "compact", privacy: .public)")
#endif
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
#if DEBUG
        ChuiniuDev.launch("viewDidAppear")
#endif
        if drawerUp {
            appeared = true
            hosting?.view.isHidden = false
        }
        becameReady()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        appeared = false
    }

    private func becameReady() {
        guard ready else { return }
        let work = afterReady
        afterReady.removeAll()
        work.forEach { $0() }
    }

    private func whenReady(_ work: @escaping () -> Void) {
        if ready { work() } else { afterReady.append(work) }
    }

    // MARK: the conversation

    private var becameActiveAt: Date?

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
#if DEBUG
        ChuiniuDev.launch("willBecomeActive")
        defer { ChuiniuDev.launch("willBecomeActive done") }
#endif
        arrived = nil
        unbound = conversation.selectedMessage == nil
        present(conversation)
        let activation = Date()
        becameActiveAt = activation
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            guard let self, self.becameActiveAt == activation, !self.ready else { return }
            self.appeared = true
            self.conversationActive = true
            self.hosting?.view.isHidden = false
            self.becameReady()
        }
    }

    override func didBecomeActive(with conversation: MSConversation) {
        super.didBecomeActive(with: conversation)
#if DEBUG
        ChuiniuDev.launch("didBecomeActive")
#endif
        conversationActive = true
        becameReady()
    }

    override func willResignActive(with conversation: MSConversation) {
        super.willResignActive(with: conversation)
        conversationActive = false
        letGo("resigning active")
    }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        super.didSelect(message, conversation: conversation)
        if let u = message.url, u == draftURL || u == sent { return }
        if let t = becameActiveAt, Date().timeIntervalSince(t) < 1 { return }
        unbound = false
        arrived = nil
        present(conversation)
    }

    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        super.didReceive(message, conversation: conversation)
        guard let url = message.url else { return }
        switch InsertStaging.receive(mine: url == staged || url == sent, staged: url == staged) {
        case .arrival: break
        case .echo, .echoOfStaged: return
        }
        arrived = url
        present(conversation)
    }

    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        super.didStartSending(message, conversation: conversation)
        voidPendingStage()
        guard let url = message.url else { return }
        // THE SENT BYTES ARE THE AUTHORITY: my staged move is committed only
        // when it is exactly what went (BridgeKernel.sent)
        host.kernel.sent(url)
        markSent(url)
#if DEBUG
        ChuiniuDev.noteSent(url)
#endif
        if message.url == draftURL { draftURL = nil; staged = nil }
        host.refresh()
        if presentationStyle != .compact || unbound { dismiss() }
    }

    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        super.didCancelSending(message, conversation: conversation)
        guard message.url == draftURL, let url = staged else { return }   // a replaced draft
        draftURL = nil
        staged = nil
        voidPendingStage()
        if host.kernel.keepsStaged(url) {
            host.kernel.cancelStaged()
            host.refresh()
        } else {
            present(conversation)
        }
    }

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
        let waiters = transitionWaiters
        transitionWaiters.removeAll()
        for seq in waiters.keys.sorted() { waiters[seq]?.resume() }
        if presentationStyle == .compact { act(loop.compact()) }
    }

    // MARK: routing

    private func present(_ conversation: MSConversation) {
        identify(conversation)
        let tapped = newest(conversation.selectedMessage?.url, arrived)
        guard let url = current(tapped) else {
            // OPENING THE APP IS THE INVITATION (uttt)
            create(in: conversation)
            return
        }
        if host.kernel.keepsStaged(url) {
            // my staged move is still the resident's: never read over it
            host.refresh()
            return
        }
        guard host.adopt(url) == 0 else { return }
        // A SUPERSEDED STAGE IS VOID (pickemup I35): a stage still resting
        // before its insert must never put the old bubble in the field
        voidPendingStage()
    }

    /// Of the selection and an arrival, the one the kernel ranks higher; an
    /// arrival from a different game never overrides what was tapped.
    private func newest(_ selected: URL?, _ arrival: URL?) -> URL? {
        guard let arrival else { return selected }
        guard let selected else { return arrival }
        guard host.kernel.sameGame(selected, arrival) else { return selected }
        return host.kernel.isNewer(selected, than: arrival) ? selected : arrival
    }

    /// This device's newest against what Messages handed over.
    private func current(_ tapped: URL?) -> URL? {
        guard let mine = staged ?? sent else { return tapped }
        guard let tapped else { return mine }
        return host.kernel.isNewer(tapped, than: mine) ? tapped : mine
    }

    private func markSent(_ url: URL) {
        if let old = sent, host.kernel.sameGame(old, url), host.kernel.isNewer(old, than: url) { return }
        sent = url
    }

    /// Who this device is: its participant id, and who sent the tapped
    /// bubble.
    private func identify(_ conversation: MSConversation) {
        // the participant's sixteen bytes, spelled out (no raw memory read)
        let u = conversation.localParticipantIdentifier.uuid
        host.kernel.me(Data([u.0, u.1, u.2, u.3, u.4, u.5, u.6, u.7, u.8, u.9, u.10, u.11, u.12, u.13, u.14, u.15]))
        if let sel = conversation.selectedMessage, let url = sel.url {
            host.kernel.sender(url, isDM: conversation.remoteParticipantIdentifiers.count == 1,
                               iSent: sel.senderParticipantIdentifier == conversation.localParticipantIdentifier)
        } else {
            host.kernel.sender(nil, isDM: false, iSent: false)
        }
    }

    /// A new lobby, me in seat 0, and its invitation staged once the drawer
    /// is up.
    private func create(in conversation: MSConversation) {
        identify(conversation)
#if DEBUG
        // the rig's full table (dev.fill): made, joined and started at once
        if let seats = ChuiniuDev.takeFill(), let bridge = host.kernel as? BridgeKernel {
            ChuiniuDev.log.info("dev.fill \(seats, privacy: .public): \(bridge.devFill(seats: seats), privacy: .public)")
            session = nil
            sessionGame = nil
            host.refresh()
            stageResident(caption: host.table.bubbleCaption, collapse: false)
            return
        }
#endif
        guard host.kernel.newGame(dm: conversation.remoteParticipantIdentifiers.count == 1) else {
            // refused (no nickname yet): the lobby asks for a name, and its
            // Join makes the lobby and stages the invitation (Kernel.newGame)
            session = nil
            sessionGame = nil
            host.unreadable = nil
            host.refresh()
            return
        }
        session = nil
        sessionGame = nil
        host.refresh()
        stageResident(caption: host.table.bubbleCaption, collapse: false)
    }

    // MARK: staging

    private var session: MSSession?
    private var sessionGame: URL?

    private func sessionFor(_ url: URL, _ conversation: MSConversation) -> MSSession {
        if let s = session, let g = sessionGame, host.kernel.sameGame(g, url) { return s }
        let s: MSSession
        if let sel = conversation.selectedMessage, let selURL = sel.url,
           let selSession = sel.session, host.kernel.sameGame(selURL, url) {
            s = selSession
        } else {
            s = MSSession()
        }
        session = s
        sessionGame = url
        return s
    }

    /// Put the resident into the input field: baked now, from the resident,
    /// before anything can load a different one into the kernel's slot.
    private func stageResident(caption: String, collapse: Bool) {
        whenReady { [weak self] in
            guard let self, let conversation = self.activeConversation else { return }
            self.stage(caption: caption, collapse: collapse, in: conversation)
        }
    }

    private func stage(caption: String, collapse: Bool, in conversation: MSConversation) {
        guard let url = host.kernel.stagedURL() else { return }
        voidPendingStage()
        let generation = stageGeneration
        let settleMs = host.kernel.stagedSettleMs

        let message = MSMessage(session: sessionFor(url, conversation))
        message.url = url
        let layout = MSMessageTemplateLayout()
        layout.image = bubbleImage(for: url)
        layout.caption = caption
        message.layout = layout
        message.summaryText = caption
        staged = url
        draftURL = message.url
#if DEBUG
        ChuiniuDev.noteStaged(url)
#endif

        guard collapse, presentationStyle != .compact else {
            insert(message, generation: generation, in: conversation)
            return
        }
        // THE DRAWER MOVES ONCE THE MOVE HAS RESTED: the kernel's staged
        // beats, then the collapse, then the bubble goes in.
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(settleMs, 0)) * 1_000_000)
            guard let self, self.stageGeneration == generation else { return }
            if self.presentationStyle != .compact {
                self.requestPresentationStyle(.compact)
                await self.awaitTransitionSettled()
            }
            guard self.stageGeneration == generation else { return }
            self.insert(message, generation: generation, in: conversation)
        }
    }

    /// The staged link's picture, drawn once a state: the same link staged
    /// again (an insert retried, a cancel and a re-stage) reuses it. Drawn
    /// from the resident at this moment, at this screen's scale; the stage's
    /// arena is freed again before this returns (TableStage.bubble).
    private var bubble: (url: URL, image: UIImage)?

    private func bubbleImage(for url: URL) -> UIImage? {
        if let b = bubble, b.url == url { return b.image }
        let image = BubbleSnapshot.render(table: host.table, title: host.word(.gameTitle),
                                          scheme: traitCollection.userInterfaceStyle == .dark ? .dark : .light,
                                          scale: traitCollection.displayScale)
        bubble = image.map { (url, $0) }
        return image
    }

    // MARK: memory

    /// AN EXTENSION HAS A HARD MEMORY LIMIT AND A WATCHDOG (foolish's
    /// procedural wood took one down on a real phone: shared/swift/Textures/
    /// WoodTexture.swift). The stage's arena is the one big block (48 MB), so
    /// it goes whenever the system asks or the extension leaves the screen:
    /// the next frame takes it again and draws the same picture (I20). The
    /// bubble picture kept for a re-stage goes too; it is drawn again if
    /// asked.
    override func didReceiveMemoryWarning() {
        super.didReceiveMemoryWarning()
        letGo("memory warning")
    }

    private func letGo(_ why: String) {
        KernelSeam.stage().purge()
        bubble = nil
#if DEBUG
        ChuiniuDev.log.info("let go of the arena: \(why, privacy: .public)")
#endif
    }

    private var hostBackground: NSObjectProtocol?

    /// The host app (Messages) going to the background is the extension
    /// going away from the screen.
    private func watchHostBackground() {
        guard hostBackground == nil else { return }
        hostBackground = NotificationCenter.default.addObserver(
            forName: .NSExtensionHostDidEnterBackground, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.letGo("host in the background") }
            }
    }

    /// Every waiter of the stage in progress sees a newer generation and
    /// gives up.
    private func voidPendingStage() {
        stageGeneration += 1
        loop.reset()
        stageInsert = nil
    }

    private func insert(_ message: MSMessage, generation: Int, in conversation: MSConversation) {
        stageInsert = (message, generation, conversation)
        act(loop.first())
    }

    private func act(_ action: InsertStaging.Loop.Action) {
        guard let s = stageInsert, s.generation == stageGeneration else { return }
        let t = loop.tryNumber
        switch action {
        case .none, .park, .landed, .door:
            return
        case .insert:
            issue(try: t, s)
        case .insertLater:
            DispatchQueue.main.asyncAfter(deadline: .now() + InsertStaging.errorBeatSeconds) { [weak self] in
                guard let self, self.stageGeneration == s.generation else { return }
                self.act(self.loop.due(try: t))
            }
        case .arm:
            watchSilence(try: t, s)
        case .revert:
            didCancelSending(s.message, conversation: s.conversation)
        }
    }

    private func issue(try t: Int, _ s: (message: MSMessage, generation: Int, conversation: MSConversation)) {
        let target = activeConversation ?? s.conversation
        watchSilence(try: t, s)
        target.insert(s.message) { [weak self] error in
            DispatchQueue.main.async {
                guard let self, self.stageGeneration == s.generation else { return }
                self.act(self.loop.answer(try: t, ok: error == nil))
            }
        }
    }

    private func watchSilence(try t: Int, _ s: (message: MSMessage, generation: Int, conversation: MSConversation)) {
        DispatchQueue.main.asyncAfter(deadline: .now() + InsertStaging.silenceSeconds) { [weak self] in
            guard let self, self.stageGeneration == s.generation else { return }
            self.act(self.loop.silence(try: t, compact: self.presentationStyle == .compact))
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
                if let waiter = self?.transitionWaiters.removeValue(forKey: id) { waiter.resume() }
            }
        }
    }
}
