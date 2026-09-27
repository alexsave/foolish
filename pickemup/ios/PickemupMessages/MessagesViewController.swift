// COPIED from uttt/ios/UtttMessages/MessagesViewController.swift at 16433dc1 - replaced by lift step S16
//
// The extension. It owns the conversation and nothing else: every rule, every
// word and every coordinate is the kernel's (pickemup/c), the screens are
// PickemupKit's, and which bubble wins, who sits where and what a bubble
// carries are pk_msg's.
//
// uttt's lifecycle, kept whole where it was learned the hard way:
//   - NOTHING IS INSERTED UNTIL THE DRAWER IS UP AND THE CONVERSATION IS
//     ACTIVE (`whenReady`): an insert before the drawer is up is silently
//     dropped on a real phone. Which appearance counts as up is shared
//     (InsertStaging.drawerUp).
//   - ONE INSERT LOOP PER STAGE (shared/c/msg_stage via InsertStaging): what
//     silence, an error and the drawer going compact mean. A stage generation
//     voids every waiter of an older stage.
//   - MY OWN BUBBLE COMING BACK THROUGH didReceive IS NOT AN ARRIVAL.
//   - markSent NEVER REBASES BACKWARDS.
//   - ONE MSSession PER GAME.
//
// What is this product's:
//   - SwiftUI, hosted (ORCHESTRATION O1), where uttt draws with Core Animation.
//   - THE RESIDENT IS ONE SLOT, and decoding is adopting. A draft of mine that
//     is still open in the kernel is never re-read from its own staged
//     (sealed) link: that would seal it and lose the undo (`keepsDraft`).
//   - WHAT WENT OUT IS THE AUTHORITY: didStartSending seals the resident draft
//     only when the sent bytes are exactly its link, and otherwise ADOPTS the
//     sent bytes (Messages has no API to take a staged bubble back, so an
//     undo to nothing can leave an older bubble in the field; if that is the
//     one sent, it is the move that happened).
//   - Messages' X on the staged bubble rebuilds the draft to its floor (D9):
//     drawn cards stay.
//   - No auto-collapse slide on render-server layers (uttt's CollapseSlide):
//     the board relays out as the drawer moves; see IOS_DECISIONS.md I10.

import Messages
import PickemupKit
import SwiftUI
import UIKit

final class MessagesViewController: MSMessagesAppViewController {

    private let host = PickemupHost()
    private var hosting: UIHostingController<PickemupRoot>?

    private var loop = InsertStaging.Loop()
    private var stageInsert: (message: MSMessage, generation: Int, conversation: MSConversation)?
    private var stageGeneration = 0

    /// The link sitting in the input field, which nobody has sent yet.
    private var staged: String?
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
        if PickemupDev.empty { return }
#endif
        view.backgroundColor = .clear
        PickemupSeats.load()
        Pk.nickname(PickemupSeats.nickname)
#if DEBUG
        if PickemupSeats.nickname.isEmpty, let nick = PickemupDev.nickname { PickemupSeats.nickname = nick }
#endif
        if !Pk.layoutMatches {
            // a stale xcframework or stale Generated/: never read at a wrong offset
            host.screen = .unreadable(PK_EFORMAT)
        }
        host.model.onStage = { [weak self] stage in self?.stageResident(stage) }
        host.onNamed = { [weak self] in
            guard let self, let c = self.activeConversation else { return }
            self.create(in: c)
        }
        let h = UIHostingController(rootView: PickemupRoot(host: host))
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
        PickemupSeats.flush()
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
        stageGeneration += 1
        loop.reset()
        stageInsert = nil
        guard let text = message.url?.absoluteString else { return }
        // THE SENT BYTES ARE THE AUTHORITY. My open draft is sealed only when
        // it is exactly what went; anything else is adopted as sent.
        if keepsDraft(text) {
            Pk.commit()
        } else {
            Pk.read(text)
        }
        markSent(text)
        if message.url == draftURL { draftURL = nil; staged = nil }
        PickemupSeats.flush()
        host.showResident()
        if presentationStyle != .compact || unbound { dismiss() }
    }

    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        super.didCancelSending(message, conversation: conversation)
        guard message.url == draftURL, let text = staged else { return }   // a replaced draft
        draftURL = nil
        staged = nil
        stageGeneration += 1
        loop.reset()
        stageInsert = nil
        if keepsDraft(text) {
            host.model.cancelStaged()                // D9: to the floor, the draws stay
            host.showResident()
        } else {
            present(conversation)
        }
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
    }

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
        let waiters = transitionWaiters
        transitionWaiters.removeAll()
        for seq in waiters.keys.sorted() { waiters[seq]?.resume() }
        if presentationStyle == .compact { act(loop.compact()) }
    }

    // MARK: routing

    /// Is `text` the link of MY draft, still open in the resident? Then the
    /// resident is the truth and the link must not be read over it.
    private func keepsDraft(_ text: String) -> Bool {
        guard let t = Pk.table(), t.draft != 0 else { return false }
        return Pk.text == text
    }

    private func present(_ conversation: MSConversation) {
        identify(conversation)
        let tapped = newest(conversation.selectedMessage?.url?.absoluteString, arrived)
        guard let text = current(tapped) else {
            // OPENING THE APP IS THE INVITATION (uttt)
            if PickemupSeats.nickname.isEmpty { host.screen = .nameGate } else { create(in: conversation) }
            return
        }
        if !keepsDraft(text) {
            let e = Pk.read(text)
            guard e == 0 else { host.screen = .unreadable(e); return }
        }
        PickemupSeats.flush()
        host.showResident()
    }

    /// Of the selection and an arrival, the one the kernel ranks higher; an
    /// arrival from a different game never overrides what was tapped.
    private func newest(_ selected: String?, _ arrival: String?) -> String? {
        guard let arrival else { return selected }
        guard let selected else { return arrival }
        guard Pk.sameGame(selected, arrival) else { return selected }
        return Pk.prefer(arrival, over: selected) <= 0 ? arrival : selected
    }

    /// This device's newest against what Messages handed over (4.8).
    private func current(_ tapped: String?) -> String? {
        guard let mine = staged ?? sent else { return tapped }
        guard let tapped else { return mine }
        return Pk.prefer(mine, over: tapped) <= 0 ? mine : tapped
    }

    private func markSent(_ text: String) {
        if let old = sent, Pk.sameGame(old, text), Pk.prefer(text, over: old) > 0 { return }
        sent = text
    }

    /// Who this device is: its participant id, and who sent the tapped bubble.
    private func identify(_ conversation: MSConversation) {
        let id = withUnsafeBytes(of: conversation.localParticipantIdentifier.uuid) { Data($0) }
        Pk.me(id)
        if let sel = conversation.selectedMessage, let text = sel.url?.absoluteString {
            Pk.sender(of: text, isDM: conversation.remoteParticipantIdentifiers.count == 1,
                      iSent: sel.senderParticipantIdentifier == conversation.localParticipantIdentifier)
        } else {
            Pk.sender(of: nil)
        }
    }

    /// A new lobby, me in seat 0, and its invitation staged once the drawer
    /// is up (creating stages by itself: UI.html Lobby 01).
    private func create(in conversation: MSConversation) {
        identify(conversation)
        guard Pk.newGame(dm: conversation.remoteParticipantIdentifiers.count == 1) else { return }
        session = nil
        sessionGame = nil
        host.showResident()
        stageResident(TableModel.Stage(caption: Pk.inviteCaption, collapse: false))
    }

    // MARK: staging

    private var session: MSSession?
    private var sessionGame: String?

    private func sessionFor(_ text: String, _ conversation: MSConversation) -> MSSession {
        if let s = session, let g = sessionGame, Pk.sameGame(g, text) { return s }
        let s: MSSession
        if let sel = conversation.selectedMessage, let selText = sel.url?.absoluteString,
           let selSession = sel.session, Pk.sameGame(selText, text) {
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
    private func stageResident(_ stage: TableModel.Stage) {
        whenReady { [weak self] in
            guard let self, let conversation = self.activeConversation else { return }
            self.stage(stage, in: conversation)
        }
    }

    private func stage(_ stage: TableModel.Stage, in conversation: MSConversation) {
        guard let text = Pk.text, let url = URL(string: text) else { return }
        stageGeneration += 1
        let generation = stageGeneration
        loop.reset()
        stageInsert = nil

        let message = MSMessage(session: sessionFor(text, conversation))
        message.url = url
        let layout = MSMessageTemplateLayout()
        layout.image = BubbleSnapshot.render(scheme: traitCollection.userInterfaceStyle == .dark ? .dark : .light)
        layout.caption = stage.caption
        message.layout = layout
        message.summaryText = stage.caption
        staged = text
        draftURL = message.url

        guard stage.collapse, presentationStyle != .compact else {
            insert(message, generation: generation, in: conversation)
            return
        }
        // THE DRAWER MOVES ONCE THE MOVE HAS RESTED (foolish: 250ms + 500ms),
        // then collapses, then the bubble goes in.
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 750_000_000)
            guard let self, self.stageGeneration == generation else { return }
            if self.presentationStyle != .compact {
                self.requestPresentationStyle(.compact)
                await self.awaitTransitionSettled()
            }
            guard self.stageGeneration == generation else { return }
            self.insert(message, generation: generation, in: conversation)
        }
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
            // .door: the shared send door is a later layer's; the bubble is
            // re-offered on the next touch that stages.
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
