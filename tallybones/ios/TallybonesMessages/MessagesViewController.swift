// COPIED from pickemup/ios/PickemupMessages/MessagesViewController.swift at 8e216923 (itself COPIED from uttt/ios/UtttMessages/MessagesViewController.swift at 16433dc1) - replaced by the conversation lift
//
// The extension. It owns the conversation and nothing else: every rule, every
// word and every die is the kernel's, reached through the TallyKernel seam
// (TallybonesKit/Kernel/TallyKernel.swift), and the screens are
// TallybonesKit's. The kernel behind the seam is BridgeKernel (tb_api.h).
//
// pickemup's lifecycle, kept whole where it was learned the hard way:
//   - NOTHING IS INSERTED UNTIL THE DRAWER IS UP AND THE CONVERSATION IS
//     ACTIVE (`whenReady`): an insert before the drawer is up is silently
//     dropped on a real phone. Which appearance counts as up is shared
//     (InsertStaging.drawerUp).
//   - ONE INSERT LOOP PER STAGE (shared/c/msg_stage via InsertStaging). A
//     stage generation voids every waiter of an older stage.
//   - MY OWN BUBBLE COMING BACK THROUGH didReceive IS NOT AN ARRIVAL.
//   - markSent NEVER REBASES BACKWARDS.
//   - ONE MSSession PER GAME.
//   - THE RESIDENT IS ONE SLOT, and decoding is adopting. A draft of mine that
//     is still open in the kernel is never re-read from its own staged link.
//   - WHAT WENT OUT IS THE AUTHORITY: didStartSending commits the resident
//     draft only when the sent bytes are exactly its link, and otherwise
//     ADOPTS the sent bytes.
//
// What is this product's (T11): a KEEP bubble is a turn's middle, not its
// end. Its send is the moment the reroll comes into existence, so
// didStartSending commits it, the tray's blanks tumble into values, and the
// drawer stays up for the next choice instead of dismissing.

import Messages
import SwiftUI
import TallybonesKit
import UIKit

final class MessagesViewController: MSMessagesAppViewController {

    private let host = TallybonesHost()
    private var kernel: TallyKernel { host.kernel }
    private var hosting: UIHostingController<TallybonesRoot>?

    private var loop = InsertStaging.Loop()
    private var stageInsert: (message: MSMessage, generation: Int, conversation: MSConversation)?
    private var stageGeneration = 0

    /// The link sitting in the input field, which nobody has sent yet.
    private var staged: String?
    /// Whether that staged link is a KEEP (the turn goes on after its send).
    private var stagedIsKeep = false
    /// The last link this device SENT.
    private var sent: String?
    /// A bubble that arrived while we were up.
    private var arrived: String?
    private var draftURL: URL?
    /// A drawer opened from the + menu is bound to no message; its first send
    /// closes it so the next tap binds it (uttt, foolish).
    private var unbound = true

    // MARK: the view

    override func viewDidLoad() {
        super.viewDidLoad()
#if DEBUG
        if TallybonesDev.empty { return }
#endif
        view.backgroundColor = .clear
#if DEBUG
        if TallybonesSeats.nickname.isEmpty, let nick = TallybonesDev.nickname {
            TallybonesSeats.set(nickname: nick, kernel: kernel)
        }
#endif
        TallybonesSeats.load(into: kernel)
        _ = host.readable
        host.model.onStage = { [weak self] stage in self?.stageResident(stage) }
        host.onNamed = { [weak self] in
            guard let self, let c = self.activeConversation else { return }
            self.create(in: c)
        }
        let h = UIHostingController(rootView: TallybonesRoot(host: host))
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
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
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

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
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

    private var becameActiveAt: Date?

    override func didBecomeActive(with conversation: MSConversation) {
        super.didBecomeActive(with: conversation)
        conversationActive = true
        becameReady()
    }

    override func willResignActive(with conversation: MSConversation) {
        super.willResignActive(with: conversation)
        TallybonesSeats.flush(kernel)
        conversationActive = false
    }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        super.didSelect(message, conversation: conversation)
        if let u = message.url, u == draftURL || u.absoluteString == sent { return }
        if let t = becameActiveAt, Date().timeIntervalSince(t) < 1 { return }
        unbound = false
        arrived = nil
        present(conversation)
    }

    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        super.didReceive(message, conversation: conversation)
        guard let text = message.url?.absoluteString else { return }
        switch InsertStaging.receive(mine: text == staged || text == sent, staged: text == staged) {
        case .arrival: break
        case .echo, .echoOfStaged: return
        }
        arrived = text
        present(conversation)
    }

    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        super.didStartSending(message, conversation: conversation)
        voidPendingStage()
        guard let text = message.url?.absoluteString else { return }
        // THE SENT BYTES ARE THE AUTHORITY. My open draft is committed only
        // when it is exactly what went; anything else is adopted as sent.
        let committed = keepsDraft(text)
        let wasKeep = committed && stagedIsKeep
        if committed {
            kernel.commit()                  // tb_api_mark_sent. T11: a KEEP's reroll exists from here
        } else {
            _ = kernel.adopt(text, arrival: false)
        }
        markSent(text)
        if message.url == draftURL { draftURL = nil; staged = nil; stagedIsKeep = false }
        TallybonesSeats.flush(kernel)
        // the kernel's send plan: the blanks that just got values settle in (T11)
        host.showResident(animate: true)
        // a KEEP is the middle of my turn: stay up for the next choice
        if wasKeep { return }
        if presentationStyle != .compact || unbound { dismiss() }
    }

    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        super.didCancelSending(message, conversation: conversation)
        guard message.url == draftURL, let text = staged else { return }   // a replaced draft
        draftURL = nil
        staged = nil
        stagedIsKeep = false
        voidPendingStage()
        if keepsDraft(text) {
            host.model.cancelStaged()
            host.showResident()
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

    /// Is `text` the link of MY draft, still open in the resident?
    private func keepsDraft(_ text: String) -> Bool {
        kernel.hasDraft && kernel.text == text
    }

    private func present(_ conversation: MSConversation) {
        identify(conversation)
        let tapped = newest(conversation.selectedMessage?.url?.absoluteString, arrived)
        guard let text = current(tapped) else {
            // OPENING THE APP IS THE INVITATION (uttt)
            if TallybonesSeats.nickname.isEmpty { host.screen = .nameGate } else { create(in: conversation) }
            return
        }
        if keepsDraft(text) {
            TallybonesSeats.flush(kernel)
            host.showResident()
            return
        }
        let e = host.adopt(text, arrival: arrived != nil)
        guard e == 0 else { host.screen = .unreadable(e); return }
        // A SUPERSEDED STAGE IS VOID (pickemup I35).
        voidPendingStage()
        TallybonesSeats.flush(kernel)
    }

    /// Of the selection and an arrival, the one the kernel ranks higher; an
    /// arrival from a different game never overrides what was tapped.
    private func newest(_ selected: String?, _ arrival: String?) -> String? {
        guard let arrival else { return selected }
        guard let selected else { return arrival }
        guard kernel.sameGame(selected, arrival) else { return selected }
        return kernel.prefer(arrival, over: selected) <= 0 ? arrival : selected
    }

    /// This device's newest against what Messages handed over.
    private func current(_ tapped: String?) -> String? {
        guard let mine = staged ?? sent else { return tapped }
        guard let tapped else { return mine }
        return kernel.prefer(mine, over: tapped) <= 0 ? mine : tapped
    }

    private func markSent(_ text: String) {
        if let old = sent, kernel.sameGame(old, text), kernel.prefer(text, over: old) > 0 { return }
        sent = text
    }

    /// Who this device is: its participant id, and who sent the tapped bubble.
    private func identify(_ conversation: MSConversation) {
#if DEBUG
        if let who = TallybonesDev.who {                 // T65: the rig plays two people on one simulator
            kernel.identify(me: TallybonesDev.participant(who))
            kernel.sender(of: nil, isDM: false, iSent: false)
            return
        }
#endif
        let id = withUnsafeBytes(of: conversation.localParticipantIdentifier.uuid) { Data($0) }
        kernel.identify(me: id)
        if let sel = conversation.selectedMessage, let text = sel.url?.absoluteString {
            kernel.sender(of: text, isDM: conversation.remoteParticipantIdentifiers.count == 1,
                          iSent: sel.senderParticipantIdentifier == conversation.localParticipantIdentifier)
        } else {
            kernel.sender(of: nil, isDM: false, iSent: false)
        }
    }

    /// A new lobby, me in seat 0, and its invitation staged once the drawer
    /// is up (creating stages by itself).
    private func create(in conversation: MSConversation) {
        guard host.readable else { return }
        identify(conversation)
        guard kernel.newGame(dm: conversation.remoteParticipantIdentifiers.count == 1) else { return }
        session = nil
        sessionGame = nil
        host.showResident()
        stageResident(TallyStage(caption: kernel.inviteCaption, collapse: false))
    }

    // MARK: staging

    private var session: MSSession?
    private var sessionGame: String?

    private func sessionFor(_ text: String, _ conversation: MSConversation) -> MSSession {
        if let s = session, let g = sessionGame, kernel.sameGame(g, text) { return s }
        let s: MSSession
        if let sel = conversation.selectedMessage, let selText = sel.url?.absoluteString,
           let selSession = sel.session, kernel.sameGame(selText, text) {
            s = selSession
        } else {
            s = MSSession()
        }
        session = s
        sessionGame = text
        return s
    }

    /// Put the resident into the input field: baked now, from the resident,
    /// before anything can load a different one into the kernel's slot.
    private func stageResident(_ stage: TallyStage) {
        whenReady { [weak self] in
            guard let self, let conversation = self.activeConversation else { return }
            self.stage(stage, in: conversation)
        }
    }

    private func stage(_ stage: TallyStage, in conversation: MSConversation) {
        guard let text = kernel.text, let url = URL(string: text) else { return }
        voidPendingStage()
        let generation = stageGeneration

        let message = MSMessage(session: sessionFor(text, conversation))
        message.url = url
        let layout = MSMessageTemplateLayout()
        // T11: a KEEP's picture has blank slots for the rerolling dice
        layout.image = host.bubbleImage(scheme: traitCollection.userInterfaceStyle == .dark ? .dark : .light)
        layout.caption = stage.caption
        message.layout = layout
        message.summaryText = stage.caption
        staged = text
        stagedIsKeep = kernel.view().draft == .keep
        draftURL = message.url

        guard stage.collapse, presentationStyle != .compact else {
            insert(message, generation: generation, in: conversation)
            return
        }
        // THE DRAWER MOVES ONCE THE MOVE HAS RESTED, then collapses, then the
        // bubble goes in.
        let settle = UInt64(max(stage.settleMs, 0)) * 1_000_000
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: settle)
            guard let self, self.stageGeneration == generation else { return }
            if self.presentationStyle != .compact {
                self.requestPresentationStyle(.compact)
                await self.awaitTransitionSettled()
            }
            guard self.stageGeneration == generation else { return }
            self.insert(message, generation: generation, in: conversation)
        }
    }

    /// Every waiter of the stage in progress sees a newer generation and gives up.
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
