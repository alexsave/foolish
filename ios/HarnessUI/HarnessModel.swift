// HarnessModel — the fake "Messages host" state for FoolishHarnessApp.
//
// NOT SHIPPING CODE (see FoolishHarnessApp.swift). It stands in for the parts of
// MSConversation the extension reads — local participant identity, the selected
// message + its sender — and for the transcript the participants send into.
//
// The one trick that makes N-way seat identity real: each pretend participant is
// bound to its OWN MessageGameStore suite (a real device has its own App Group).
// So when you "become" another player, seat inference resolves off THAT player's
// empty cache + the sender identity, exactly as two real phones would.
//
// A SECOND trick, added for the chat-scoping security fix: the harness also
// simulates TWO conversations (`chats: [ChatState]`, below), each with its own
// transcript and its own `chatKey`, both readable by whichever participant is
// currently "you". Switching chats does NOT rebind the store — a real phone's
// App Group is shared across every conversation, only the conversation identity
// (`chatKey`) differs — which is exactly the precondition for the bug this fix
// closes: stage a game as "You" in Chat A, switch to Chat B as "You" with
// nothing selected, and the harness must land on New game, never reopen
// Chat A's board. (See MessageGameStore's type doc for the underlying leak.)

import SwiftUI
import UIKit
import FoolishKit

@MainActor
final class HarnessModel: ObservableObject {
    struct Participant: Identifiable, Equatable { let id = UUID(); let name: String }
    /// One delivered transcript entry. `preview` is the bubble IMAGE, snapshotted
    /// once when the move was staged — exactly what a real MSMessage carries
    /// (Messages snapshots the extension at insert time and the picture never
    /// changes afterwards). It is stored, not re-derived, for a correctness
    /// reason and not just fidelity: the harness's transcript used to render each
    /// bubble by mounting a LIVE MessagesRootView pointed at that bubble's
    /// payload, and every one of those decoded its payload into the single static
    /// resident kernel that the open board is also using. Old bubbles therefore
    /// re-adopted stale chains (an early 8-seat lobby among them) under the live
    /// board's feet, wrote their own rows into the seat cache, and ran their own
    /// animation streams — which is how two participants ended up looking at
    /// different games, and why a pickup animated twice.
    struct Msg: Identifiable, Equatable {
        let id = UUID(); let url: URL; let senderId: UUID; let senderName: String
        let preview: UIImage?
    }

    /// One simulated conversation's mutable state: its own transcript and its
    /// own "am I starting a fresh game" flag, exactly the two things that must
    /// never leak across chats. `key` stands in for `MSConversation
    /// .localParticipantIdentifier.uuidString` — fixed for the app's lifetime,
    /// distinct between the two slots, same shape as the real per-conversation
    /// UUID.
    private struct ChatState {
        let key = UUID().uuidString
        var transcript: [Msg] = []
        var startNewGame = true
        /// Which bubble the extension is opened against — real Messages'
        /// `MSConversation.selectedMessage`. nil means "the newest", which is
        /// what a fresh open gets. Tapping an OLDER bubble sets it, exactly like
        /// tapping an older bubble on a phone: the extension opens on that
        /// chain, and Rule P decides whether it is still the one to show.
        var selected: UUID?
    }

    @Published private(set) var participants: [Participant]
    @Published private(set) var localIndex = 0
    /// Two simulated conversations, index 0 ("Chat A") and 1 ("Chat B"). Every
    /// participant can be "you" in either — see the type doc above.
    @Published private var chats: [ChatState] = [ChatState(), ChatState()]
    /// Which of `chats` is currently open.
    @Published private(set) var currentChat = 0
    /// The bubble the board auto-staged (the extension's `insert`), not yet
    /// delivered. Stands in for the Messages input field: the harness Send button
    /// is the blue send arrow. nil = nothing staged.
    @Published private(set) var staged: Data?
    /// The staged bubble's picture, snapshotted at stage time like `Msg.preview`.
    @Published private(set) var stagedPreview: UIImage?
    /// Simulated Messages presentation style. The real extension collapses to the
    /// compact drawer (and Messages' Send) once a move is staged; the harness has no
    /// Messages host, so it fakes the same expanded<->compact transition here so the
    /// collapse flow is testable. `.expanded` = full board; `.compact` = drawer.
    @Published private(set) var presentation: MsgPresentation = .expanded
    /// The drawer has been swiped fully away (real Messages' third gesture:
    /// swipe down again from compact). Lives HERE, not in the view, because
    /// `presentation` and "is the drawer even on screen" have to be read
    /// TOGETHER — see `stageIsExpanded` for the dead end that came of keeping
    /// them apart.
    @Published private(set) var drawerDismissed = false

    /// Is the drawer covering the screen right now? A DISMISSED drawer is never
    /// expanded, whatever `presentation` says.
    ///
    /// The bug this closes: `presentation` and dismissal were independent, and
    /// the chrome hid the compose bar whenever `presentation == .expanded`. But
    /// every one of `expand`/`become`/`switchChat`/`newGame`/`setCount` sets
    /// `.expanded` unconditionally — so dismissing the drawer and then touching
    /// any of them left BOTH hidden: no drawer (dismissed) and no compose bar
    /// (nominally expanded), i.e. an empty chat with no way back in, since the
    /// only way back is the compose bar's "+". Owner: "after I fully collapsed
    /// the extension view so that it wouldn't appear, it was just gone."
    var stageIsExpanded: Bool {
        if drawerDismissed { return false }
        switch presentation { case .expanded: return true; case .compact: return false }
    }
    /// Swipe-down-again from the compact drawer: put it away entirely.
    func dismissDrawer() { drawerDismissed = true }
    /// The compose bar's "+" — real Messages' app-drawer icon, and the only way
    /// back to a dismissed drawer.
    func reopenDrawer() { drawerDismissed = false }
    /// DEV diagnostic surfaced in the chrome (seat/turn/legal after a seed).
    @Published private(set) var debugInfo = ""

    // "You" was the first name here; the owner asked for a real one — with eight
    // participants and a "you are:" switcher above them, a player literally named
    // "You" reads as the label rather than as a person.
    private static let names = ["Alex", "Vera", "Boris", "Dima", "Katya", "Lev", "Mila", "Oleg"]

    init(count: Int? = nil) {
        let env = Int(ProcessInfo.processInfo.environment["HARNESS_PLAYERS"] ?? "")
        participants = Self.make(count ?? env ?? 2)
        // DEV: start in the compact drawer. It is otherwise reachable only by
        // dragging the grabber, which headless screenshotting cannot do — and
        // compact is exactly the state where the compose bar's position has to
        // be checked. Same family as HARNESS_SEED/HARNESS_PLAYERS.
        if ProcessInfo.processInfo.environment["HARNESS_COMPACT"] != nil {
            presentation = .compact
        }
        rebindStore()
    }

    /// DEV pacing multiplier (HARNESS_PACE, default 1). Every deliberate wait
    /// in the auto-played game is scaled by this, so a run can be slowed down
    /// to human-watchable speed without touching the beats' relative timing —
    /// the point of an unattended run is usually to WATCH it, and at 1x the
    /// turns go by faster than anyone can take notes on.
    static let pace: Double = Double(ProcessInfo.processInfo.environment["HARNESS_PACE"] ?? "") ?? 1
    /// `seconds`, scaled by `pace`, as nanoseconds for Task.sleep.
    static func beat(_ seconds: Double) -> UInt64 { UInt64(seconds * pace * 1_000_000_000) }

    private static func make(_ n: Int) -> [Participant] {
        (0..<max(2, min(8, n))).map { Participant(name: names[$0]) }
    }

    // MARK: derived inputs for MessagesRootView

    var localId: UUID { participants[localIndex].id }
    var localName: String { participants[localIndex].name }
    var playerCount: Int { participants.count }
    /// This conversation's identity — threaded into MessagesRootView exactly as
    /// MessagesViewController threads `conversation.localParticipantIdentifier
    /// .uuidString`.
    var chatKey: String { chats[currentChat].key }
    var transcript: [Msg] { chats[currentChat].transcript }
    var startNewGame: Bool { chats[currentChat].startNewGame }
    var latest: Msg? { transcript.last }
    /// The bubble the extension is opened against — the tapped one, else the
    /// newest (see `ChatState.selected`).
    var selectedMsg: Msg? {
        if let id = chats[currentChat].selected, let m = transcript.first(where: { $0.id == id }) {
            return m
        }
        return latest
    }
    /// The bubble the extension is presented against — routed through the SAME
    /// `StagedBubbleRouting` the real extension uses, which the harness simply
    /// did not have.
    ///
    /// Sending used to change this: `deliver()` appends the bubble and clears
    /// the selection, so this became the NEW message's URL, `GameSurface
    /// .loadKey` changed, the live controller was torn down and rebuilt from
    /// the chain I had just sent — and replayed the move I had just watched
    /// myself play. On a real device `MessagesViewController` already refuses
    /// that (`lastSentPayload`); here it went unnoticed because the auto-game
    /// switches player immediately after delivering, which hides it.
    ///
    /// So: my own bubble, staged or just sent, keeps presenting the URL already
    /// in use. The board stays exactly as the move left it.
    var payloadURL: URL? {
        let selected = startNewGame ? nil : selectedMsg?.url
        // Nothing presented yet means there is no board to protect — the very
        // first send in a thread goes setup -> board, and pinning to a nil
        // "URL already in use" would route it to the New game screen instead
        // (or, worse, to "this game link is damaged").
        guard let presented = presentedURL else { return selected }
        return StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: selected,
            startingNewGame: startNewGame,
            pendingStage: staged.map { (payload: $0, mySeat: localIndex) },
            lastPayloadURL: presented,
            lastSentPayload: lastSentPayload)
    }
    /// The URL currently being presented — the `lastPayloadURL` above. Updated
    /// by every action that genuinely changes which chain is on screen, and
    /// deliberately NOT by `deliver()`.
    private var presentedURL: URL?
    /// The payload this device most recently sent, so its own bubble arriving
    /// as the selection is recognised rather than adopted.
    private var lastSentPayload: Data?

    /// Re-point `presentedURL` at whatever is selected now. Called by the
    /// actions that really do change the chain on screen (a tapped bubble, a
    /// different player, a different chat, New game) — never by `deliver`.
    private func rememberPresented() {
        presentedURL = startNewGame ? nil : selectedMsg?.url
    }
    /// Did the CURRENT player send the bubble being opened? Drives §6.2 sender
    /// inference — and it must follow the SELECTION, not the transcript's tail,
    /// or tapping an older bubble would resolve identity off a different message
    /// than the one being decoded.
    var senderIsLocal: Bool { selectedMsg?.senderId == localId }
    /// `staged`, wrapped the same way a delivered bubble's `Msg.url` already is.
    /// Kept for parity/debugging only — the transcript renders `stagedPreview`,
    /// the snapshot, and no longer mounts a board per bubble (see `Msg`).
    var stagedURL: URL? { staged.map { MessageEnvelope.link(payload: $0) } }
    var chatIsDM: Bool { participants.count == 2 }
    /// Bumped ONLY when the board must genuinely be rebuilt: a different
    /// player, a different chat, an explicitly tapped bubble, a New game.
    ///
    /// Deliberately NOT bumped by `deliver()`, and this is the whole point.
    /// Sending used to change `transcript.count`, which changed `viewKey`,
    /// which tore the live board down and rebuilt it from the bubble I had
    /// just sent — so the move I had just watched myself play was replayed as
    /// an open-replay. That is the "double pickup animation": one live, one
    /// from the rebuild. A real device doesn't do this either (the extension's
    /// StagedBubbleRouting keeps presenting the same URL when the newly
    /// selected message is my own just-staged bubble), the harness simply had
    /// no equivalent. The board already shows that exact chain — it is my own
    /// move — so there is nothing to reload.
    @Published private(set) var boardEpoch = 0

    /// Reset MessagesRootView's @State whenever the player, the chat, the
    /// selected bubble, or the new-game intent changes — see `boardEpoch` for
    /// what deliberately does NOT count.
    var viewKey: String { "\(localIndex)-\(currentChat)-\(boardEpoch)" }

    /// Tap a transcript bubble: open the extension on THAT message, the way
    /// tapping a bubble does on a phone. Also brings the drawer back — a tap on
    /// a game bubble is unambiguously "show me this game", and with the drawer
    /// dismissed there is otherwise nothing on screen to tap but the bubbles.
    func openBubble(_ msg: Msg) {
        boardEpoch += 1
        chats[currentChat].selected = msg.id
        chats[currentChat].startNewGame = false
        staged = nil; stagedPreview = nil
        presentation = .expanded
        drawerDismissed = false
        rememberPresented()
    }

    // MARK: chat list (req 2 of the Messages-host redesign)
    //
    // The old chrome had a two-button "Chat A"/"Chat B" switcher living next to
    // the participant strip. Real Messages has no such thing — you reach another
    // conversation by tapping the back chevron in the nav bar, which shows the
    // conversation LIST, then tapping a row. `chatSummaries` is what powers that
    // list screen (ChatListView.swift); it stays read-only derived state so the
    // list can never drift from `chats` itself.

    /// One row in the simulated chat list.
    struct ChatSummary: Identifiable, Equatable {
        let id: Int              // 0 ("Chat A") or 1 ("Chat B") — same indices `switchChat` takes
        let label: String        // e.g. "Vera chat A" or "Group chat B"
        let preview: String      // last bubble's sender, or an empty-state string
        let isCurrent: Bool
    }

    /// Who the chat list / nav bar should show as "the contact" — i.e. everyone
    /// in this simulated device's conversations EXCEPT whichever participant is
    /// currently "you" (see `become`). A DM names the other participant, exactly
    /// like a real 1:1 thread's header; 3+ reads as "Group", matching the owner's
    /// own example strings ("Vera chat A" / "Group chat A") rather than listing
    /// every name (real Messages group headers are usually a short "Group Name"
    /// too, not the full roster).
    var contactLabel: String {
        guard chatIsDM else { return "Group" }
        return participants.first(where: { $0.id != localId })?.name ?? "?"
    }

    /// Avatar initials for `contactLabel` (mirrors the reference screenshot's
    /// "KB" circle) — first letter of up to two words.
    var contactInitials: String {
        let words = contactLabel.split(separator: " ")
        if words.count >= 2 { return "\(words[0].prefix(1))\(words[1].prefix(1))".uppercased() }
        return String(contactLabel.prefix(2)).uppercased()
    }

    /// The two simulated conversations, described for the chat list. Reads
    /// `chats` directly (private to this file) without exposing it — the list
    /// screen only ever needs id/label/preview/isCurrent, never the transcript
    /// or startNewGame flag of a chat that isn't open.
    var chatSummaries: [ChatSummary] {
        (0..<chats.count).map { idx in
            let letter = idx == 0 ? "A" : "B"
            let preview = chats[idx].transcript.last.map { "\($0.senderName) sent a move" }
                ?? "No messages yet"
            return ChatSummary(id: idx, label: "\(contactLabel) chat \(letter)",
                               preview: preview, isCurrent: idx == currentChat)
        }
    }

    // MARK: actions

    /// Change the number of pretend participants and start over (both chats).
    func setCount(_ n: Int) {
        participants = Self.make(n)
        boardEpoch += 1
        chats = [ChatState(), ChatState()]
        localIndex = 0
        currentChat = 0
        presentation = .expanded
        drawerDismissed = false
        lastSentPayload = nil
        rememberPresented()
        rebindStore()
    }

    /// The compact drawer's "Open the game" (the extension's requestPresentationStyle).
    /// Un-dismisses too: asking for the game means showing it.
    func expand() { presentation = .expanded; drawerDismissed = false }
    /// Manual expand/collapse for poking at the compact drawer in the harness.
    func togglePresentation() { presentation = presentation == .expanded ? .compact : .expanded }

    /// "Become" participant `idx` — the crux of the harness. Rebinds the seat
    /// cache to that participant's own suite and reads the latest transcript
    /// bubble as them (senderIsLocal recomputes, so they are the receiver).
    /// Does NOT change which chat is open — becoming someone else is a separate
    /// axis from switching conversations (see `switchChat`).
    func become(_ idx: Int) {
        guard idx >= 0, idx < participants.count else { return }
        AnimLog.say("host become \(participants[idx].name) transcript=\(transcript.count) selected=\(chats[currentChat].selected != nil)")
        // Discard the current player's half-staged move. (ROUND 9: the durable
        // pending ledger this also had to clear is gone entirely - owner call.)
        boardEpoch += 1
        localIndex = idx
        // Route to New game when this player has no bubble to open yet (switching
        // before anyone has delivered). Only a real, delivered bubble reads as a
        // game; (startNewGame=false, no bubble) is the "game link is damaged"
        // screen, which is wrong here — there is simply nothing sent to them.
        chats[currentChat].startNewGame = (latest == nil)
        chats[currentChat].selected = nil   // a new player opens on the newest bubble
        staged = nil; stagedPreview = nil   // a half-staged move doesn't cross to another player
        presentation = .expanded     // opening the game as this player
        drawerDismissed = false
        lastSentPayload = nil        // a different device never sent my bubble
        rememberPresented()
        rebindStore()
    }

    /// Switch which of the two simulated conversations is open, WITHOUT
    /// rebinding the store: this is the harness's stand-in for tapping a
    /// different thread's iMessage extension on the SAME phone — see the type
    /// doc's second trick. `become`'s pending-ledger discard does not apply
    /// here (nothing about switching conversations touches a device's staged
    /// move state on a real phone either), but the currently staged bubble is
    /// still cleared: it stands in for the Messages compose field, which is
    /// per-conversation and never carries over when you leave a thread.
    func switchChat(_ idx: Int) {
        guard idx == 0 || idx == 1, idx != currentChat else { return }
        boardEpoch += 1
        currentChat = idx
        staged = nil; stagedPreview = nil
        presentation = .expanded
        drawerDismissed = false
        lastSentPayload = nil
        rememberPresented()
    }

    /// The current player tapped New game. New game always opens full-screen.
    func newGame() {
        boardEpoch += 1
        chats[currentChat].startNewGame = true; staged = nil; stagedPreview = nil
        presentation = .expanded; drawerDismissed = false
        lastSentPayload = nil
        rememberPresented()
    }

    /// Retract the staged bubble (§10 undo, batch-1 note 32): an undo that empties
    /// the pending ledger must drop the Send-lit payload too, not just no-op and
    /// leave a stale move ready to send. Stands in for the real extension's
    /// `pendingStage = nil` — the harness has no inserted-bubble UI to remove.
    func unstage() { staged = nil; stagedPreview = nil }

    /// The board auto-staged a chain (the extension's `insert`). Hold it; the
    /// human still has to press Send — that is `deliver()`.
    ///
    /// Staging must NOT touch `startNewGame`: `viewKey` keys the live
    /// MessagesRootView on it, so flipping it here would tear down the very board
    /// that just staged the move (it reloads with payloadURL=nil → "damaged").
    /// The new-game intent only clears when the bubble is actually delivered.
    func stage(_ payload: Data, seat: Int, fromUndo: Bool = false) async {
        AnimLog.say("host stage seat=\(seat) fromUndo=\(fromUndo)")
        staged = payload
        // Snapshot the bubble picture NOW, the way MessagesViewController.stage
        // does — the resident kernel is this payload at exactly this moment (the
        // board just sealed it), and the same BubbleSnapshot entry picks lobby vs
        // board. Every later render of this bubble is that image, so nothing in
        // the transcript ever touches the kernel again (see `Msg.preview`).
        stagedPreview = await Self.snapshot(payload)
        // Mirror the real extension: let the move's animation play out (card flight,
        // and for a bout-ending good the discard + draws), rest ~500ms so the result
        // reads, THEN collapse to the compact drawer (which shows the staged game).
        Task { [weak self] in
            // The SAME wait the real extension does (round-2 note 8), which the
            // harness never got: a flat 900ms guillotines a bout-end cascade
            // mid-flight, and collapsing resizes the board out from under the
            // in-flight cards, so they land against stale rects — the "glitchy"
            // half of the pickup complaint. Short lead-in first (the sequence
            // starts inside a Task the onChange schedules, so checking
            // isSequencing with no lead-in can race it), then wait for however
            // long the real sequence takes, then rest so the result reads.
            try? await Task.sleep(nanoseconds: Self.beat(0.25))
            await BoardAnimator.waitForSettle()
            try? await Task.sleep(nanoseconds: Self.beat(0.5))
            guard let self, self.staged != nil else { return }
            // Skip the collapse during an auto-played game so our move's animation
            // finishes on the full board (it's the point of the slow run); real
            // interactive staging still collapses to Messages' Send.
            //
            // HARNESS_AUTOGAME_COLLAPSE forces it anyway: the collapse is the
            // ONE thing an auto-played game does not do that a human does, so
            // reproducing an "it animated twice when I tapped it myself" report
            // needs it in the loop.
            // An undo re-stage must never collapse - it keeps the expanded board up
            // so the player can pick a different move (mirrors the real extension's
            // `fromUndo` guard in MessagesViewController.stage).
            if !fromUndo,
               ProcessInfo.processInfo.environment["HARNESS_NOCOLLAPSE"] == nil,
               ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] == nil
                || ProcessInfo.processInfo.environment["HARNESS_AUTOGAME_COLLAPSE"] != nil {
                AnimLog.say("host collapse -> compact")
                self.presentation = .compact
            }
        }
        // HARNESS_AUTOGAME: auto-deliver + hand to the next player, so a whole
        // game plays itself through the real UI (validates turn handoff + seat
        // inference across many turns). Halts naturally at game over (a finished
        // board auto-plays no move → nothing to deliver).
        if ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] != nil {
            Task { [weak self] in
                // Let the move animate and any bout-end sequence START…
                try? await Task.sleep(nanoseconds: Self.beat(1.4))
                // …then wait for the WHOLE bout-end sequence (discard/pickup + each
                // player's draw, one at a time) to finish before switching users, so
                // the beat reads: open → their move replays → we act → our move +
                // the full discard/draw cascade animate → switch.
                while BoardAnimator.isSequencing { try? await Task.sleep(nanoseconds: 150_000_000) }
                try? await Task.sleep(nanoseconds: Self.beat(0.6))   // rest so the settled board reads
                guard let self, self.staged != nil else { return }
                self.deliver()
                await self.becomeSomeoneWhoCanMove()
            }
        }
    }

    /// HARNESS_AUTOGAME's handoff: become the next participant who actually has
    /// a legal move, searching round-robin from the current one.
    ///
    /// It used to be a flat `(localIndex + 1) % count`, which works in a DM and
    /// stalls at three or more from the very first turn: only the first
    /// attacker can act on an empty table, so handing the game to whoever
    /// happens to be seated next usually hands it to a seat whose only legal
    /// move is `wait`. That seat stages nothing, so nothing is ever delivered,
    /// and the run sits there — which is why an 8-player auto-run produced four
    /// lines of trace and no game.
    ///
    /// The kernel answers "can this seat act" (`residentLegal`), so nothing
    /// here guesses at turn order. Falls back to the plain next seat when
    /// nobody can move, so a finished game still advances rather than hanging.
    private func becomeSomeoneWhoCanMove() async {
        let n = participants.count
        // While the thread is still a LOBBY there is no turn order to consult —
        // the resident game is the deal at the lobby's capacity, whose "first
        // attacker" is a phantom of a game nobody is playing yet. Asking it
        // would jump straight past everyone who still has to join. Plain
        // round-robin until someone starts the game.
        if let latest = self.latest,
           let bytes = try? MessageEnvelope.payloadBytes(url: latest.url),
           let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1),
           env.phase == 0 {
            become((localIndex + 1) % n)
            return
        }
        // The just-delivered chain is the resident game (deliver() re-adopted
        // nothing, but the board that sealed it left it resident), so this asks
        // about the position everyone is actually looking at.
        // "Can act" must mean what the BOARD means by it, not what the kernel
        // menu says — the kernel always offers `good`, the board only offers it
        // once every attack is covered. Handing the game to a seat whose sole
        // offer is a `good` it is not allowed to make stops the run dead on a
        // board with no live button (observed at 8 players: an attacker with
        // nothing to throw in, two uncovered battles, and the game frozen).
        // The defender can always still cover or take, so the game is never
        // actually stuck — only this handoff was.
        for step in 1...n {
            let seat = (localIndex + step) % n
            let legal = await MessageKernel.shared.residentLegal(seat: seat)
            guard let view = await MessageKernel.shared.residentView(viewer: seat) else { continue }
            if !CardPlay.humanMoves(battles: view.battles, legal: legal).isEmpty {
                become(seat); return
            }
        }
        become((localIndex + 1) % n)
    }

    #if DEBUG
    /// Blink repro (HARNESS_RECEIVE_LIVE): a bubble arrives from ANOTHER seat
    /// while THIS viewer stays put and expanded. Unlike `become` (a full
    /// boardEpoch/viewKey remount that models a different device opening the
    /// app), this is the REAL live receive: the transcript grows, so the active
    /// board's `payloadURL` moves to the new bubble and its `loadKey` reloads -
    /// with NO boardEpoch bump. That is exactly the case whose reload used to
    /// flash `Color.clear`. Headlessly seals the other seat's first legal move
    /// off the resident game (a throwaway controller); the mounted board's own
    /// controller keeps its cached view until the reload swaps it.
    func simulateLiveReceive() async {
        guard let latest = self.latest,
              let bytes = try? MessageEnvelope.payloadBytes(url: latest.url),
              let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else {
            AnimLog.say("host simulateLiveReceive: no bubble"); return
        }
        let n = participants.count
        for step in 1...n {                       // prefer a seat that is NOT me
            let seat = (localIndex + step) % n
            let legal = await MessageKernel.shared.residentLegal(seat: seat)
            guard let view = await MessageKernel.shared.residentView(viewer: seat),
                  let move = CardPlay.humanMoves(battles: view.battles, legal: legal).first else { continue }
            let ctrl = MessageTurnController(parentPayload: bytes, parent: env, mySeat: seat)
            await ctrl.begin()
            await ctrl.apply(move)
            guard let payload = try? await ctrl.stagedPayload() else { continue }
            chats[currentChat].transcript.append(Msg(url: MessageEnvelope.link(payload: payload),
                                                     senderId: participants[seat].id,
                                                     senderName: participants[seat].name,
                                                     preview: nil))
            chats[currentChat].selected = nil
            lastSentPayload = nil                 // I did not send it -> payloadURL moves -> loadKey reload
            AnimLog.say("host simulateLiveReceive from \(participants[seat].name) seat=\(seat)")
            return
        }
        AnimLog.say("host simulateLiveReceive: no seat could move")
    }
    #endif

    /// The blue send arrow: deliver the staged bubble into the shared transcript
    /// so the next participant can read it.
    func deliver() {
        guard let payload = staged else { return }
        AnimLog.say("host deliver")
        // What the board is showing RIGHT NOW, captured before the transcript
        // grows — this is the URL it keeps presenting once my own bubble lands
        // in the thread, so sending does not reload it. Read before `append`
        // for the obvious reason: afterwards `selectedMsg` is my new bubble.
        presentedURL = payloadURL
        chats[currentChat].transcript.append(Msg(url: MessageEnvelope.link(payload: payload),
                                                 senderId: localId, senderName: localName,
                                                 preview: stagedPreview))
        staged = nil
        stagedPreview = nil
        // Recognise my own bubble when it comes back as the selection, so the
        // board I am looking at survives the send untouched (see payloadURL).
        lastSentPayload = payload
        chats[currentChat].selected = nil     // the newest bubble is the selection again
        // Now that a real bubble exists, leave the new-game screen: the reload
        // this delivery triggers (transcript.count changed) must read the bubble,
        // not route back to setup. (Was done in stage(); see the note there.)
        chats[currentChat].startNewGame = false
        // ROUND 9: mirror the real didStartSending - the pending-ledger clear is
        // gone with the ledger itself (owner call); the just-sent marker is what
        // keeps a reload of my OWN chain from replaying my move back at me.
        MessageGameStore.shared.markJustSent(payload: payload)
    }

    /// REVIEW RIG (HarnessScenario.swift): drop a bubble into the open
    /// transcript that this device did NOT seal and cannot decode — the
    /// corrupt/foreign-link path. `chats` is private to this file, so this
    /// lives here rather than in the scenario extension.
    func appendForeignBubble(url: URL) {
        boardEpoch += 1
        chats[currentChat].transcript.append(Msg(url: url, senderId: participants[min(1, participants.count - 1)].id,
                                                 senderName: participants[min(1, participants.count - 1)].name,
                                                 preview: nil))
        chats[currentChat].startNewGame = false
        chats[currentChat].selected = nil
        staged = nil; stagedPreview = nil
        lastSentPayload = nil
        rememberPresented()
    }

    /// The bubble picture for a sealed chain — the SAME `BubbleSnapshot` entry
    /// the real extension composes its MSMessage image with, so the harness's
    /// transcript shows what a real thread would show. Decoding here re-adopts
    /// the payload into the resident kernel, which is what the real extension's
    /// `stage` does too and is safe for the same reason: it is the chain the
    /// board just sealed, so "re-adopt" is a no-op on the state.
    private static func snapshot(_ payload: Data) async -> UIImage? {
        guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1)
        else { return nil }
        return await BubbleSnapshot.render(env: env)
    }

    // Each participant → its own throwaway cache suite (fresh per app launch via
    // the per-run UUIDs), so no seat leaks between "devices".
    private func rebindStore() {
        MessageGameStore.shared = MessageGameStore(suiteName: "fmsg.harness.\(localId.uuidString)")
        // DEV: during an auto-played game, pre-name each pretend player so the
        // one-time name gate never blocks the unattended run.
        if ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] != nil {
            MessageGameStore.shared.nickname = localName
        }
    }

    /// DEV screenshotting: deal a real 2-player game, play seat 0's opening
    /// attack, and land as the receiver (Vera) viewing that bubble — so the board
    /// renders without any taps. Gated by the HARNESS_SEED launch env; never runs
    /// in normal use.
    func seedDemoGame() async {
        let n = participants.count
        guard n >= 2 else { return }
        // bug-2 crux: can a 0-move GENESIS (1 join, parent=zeros) be sealed? If so
        // the creator-not-first-attacker can send the deal (MessageTableView.stageNow).
        try? await MessageKernel.shared.newGame(seed: Data(repeating: 3, count: 32), players: 2)
        let g0seal = ((try? await MessageKernel.shared.seal(
            phase: 2, lastActorSeat: 0, gameId: 0xBEEF,
            parent8: Data(repeating: 0, count: 8),
            joins: [MessageJoin(seat: 0, name: "You")])) != nil)
        let seed = Data(repeating: 42, count: 32)   // fixed → reproducible screenshots
        let gid: UInt64 = 0xF001
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: n)
            // A fresh LIVE handoff: everyone seated, no move yet. Whoever holds
            // the lowest trump is the first attacker.
            let joins = (0..<n).map { MessageJoin(seat: $0, name: participants[$0].name) }
            var payload = try await MessageKernel.shared.seal(
                phase: 2, lastActorSeat: 0, gameId: gid,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            _ = try? await MessageKernel.shared.decode(payload: payload, viewer: -1)

            // DEV: HARNESS_SEED_PLAY=N applies N attack/cover steps (cover an
            // uncovered attack when possible, else attack) so the board renders a
            // populated, wrapping table for layout screenshots — then re-seals it.
            if let raw = ProcessInfo.processInfo.environment["HARNESS_SEED_PLAY"] {
                let steps = Int(raw) ?? 6
                var lastSeat = 0
                for _ in 0..<steps {
                    var acted = false
                    for s in 0..<n {
                        let legal = await MessageKernel.shared.residentLegal(seat: s)
                        if let cover = legal.first(where: { $0.type == .cover }) {
                            try? await MessageKernel.shared.apply(seat: s, move: cover)
                            lastSeat = s; acted = true; break
                        }
                    }
                    if !acted {
                        for s in 0..<n {
                            let legal = await MessageKernel.shared.residentLegal(seat: s)
                            if let atk = legal.first(where: { $0.type == .attack }) {
                                try? await MessageKernel.shared.apply(seat: s, move: atk)
                                lastSeat = s; acted = true; break
                            }
                        }
                    }
                    if !acted { break }
                }
                if let p = try? await MessageKernel.shared.seal(
                    phase: 2, lastActorSeat: lastSeat, gameId: gid,
                    parent8: Data(repeating: 0, count: 8), joins: joins) {
                    payload = p
                    _ = try? await MessageKernel.shared.decode(payload: payload, viewer: -1)
                }
            }
            // DEV: HARNESS_ENDSCREEN plays the whole game out (first legal move for
            // any actionable seat) until it is over, then seals FINISHED so the
            // board lands on the ranked end screen — a deterministic screenshot.
            if ProcessInfo.processInfo.environment["HARNESS_ENDSCREEN"] != nil {
                var guardN = 0
                while (await MessageKernel.shared.residentView(viewer: -1))?.isOver != true, guardN < 6000 {
                    guardN += 1
                    var acted = false
                    for s in 0..<n {
                        let legal = await MessageKernel.shared.residentLegal(seat: s)
                        if let m = legal.first(where: { $0.type != .wait }) {
                            try? await MessageKernel.shared.apply(seat: s, move: m)
                            acted = true; break
                        }
                    }
                    if !acted { break }
                }
                if let p = try? await MessageKernel.shared.seal(
                    phase: 3, lastActorSeat: 0, gameId: gid,
                    parent8: Data(repeating: 0, count: 8), joins: joins) {
                    payload = p
                    _ = try? await MessageKernel.shared.decode(payload: payload, viewer: -1)
                }
            }
            var actor = 0
            for s in 0..<n where (await MessageKernel.shared.residentLegal(seat: s)).contains(where: { $0.type != .wait }) {
                actor = s; break
            }
            let la = await MessageKernel.shared.residentLegal(seat: actor)
            let v = await MessageKernel.shared.residentView(viewer: actor)
            debugInfo = "g0seal=\(g0seal) n=\(n) def=\(v?.defender ?? -9) firstActor=\(actor) legal=[\(la.map { "\($0.type)" }.joined(separator: ","))]"

            // Always seeds into chat 0 ("Chat A") — a dev screenshot helper, not the
            // two-chat leak repro, so it doesn't need to honor `currentChat`.
            let shot = await Self.snapshot(payload)
            chats[0] = ChatState(transcript: [Msg(url: MessageEnvelope.link(payload: payload),
                                                  senderId: participants[0].id,
                                                  senderName: participants[0].name,
                                                  preview: shot)],
                                 startNewGame: false)
            currentChat = 0
            localIndex = actor           // view as the actionable seat → the board shows
            rebindStore()
            // The seed demo knows every name, so give the viewer theirs — otherwise
            // the one-time name gate intercepts before the board (this is a
            // screenshot helper, not the join flow).
            MessageGameStore.shared.nickname = participants[actor].name
            // N>=3: a non-sender with no cache is §6.3 ambiguous (would show the
            // seat picker). Pre-cache the viewer's seat so the board shows directly.
            if let env = try? await MessageEnvelope.decode(payload: payload, viewer: actor) {
                let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
                MessageGameStore.shared.put(MessageGameRecord(
                    gameId: env.gameId, chatKey: chatKey, mySeat: actor, nPlayers: env.nPlayers, round: env.round,
                    turn: env.turn, phase: env.phase, finished: false, names: names,
                    payloadBase32: Base32.encode(payload), updatedAt: 1))
            }
        } catch {
            // leave the harness on the New-game screen if seeding fails
        }
    }
}
