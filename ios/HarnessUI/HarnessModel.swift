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
    var payloadURL: URL? { startNewGame ? nil : selectedMsg?.url }
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
    /// Reset MessagesRootView's @State whenever the player, the chat, the
    /// transcript, or the new-game intent changes, so it re-derives as the
    /// current participant IN the current chat.
    var viewKey: String {
        "\(localIndex)-\(currentChat)-\(transcript.count)-\(startNewGame)-\(selectedMsg?.id.uuidString ?? "")"
    }

    /// Tap a transcript bubble: open the extension on THAT message, the way
    /// tapping a bubble does on a phone. Also brings the drawer back — a tap on
    /// a game bubble is unambiguously "show me this game", and with the drawer
    /// dismissed there is otherwise nothing on screen to tap but the bubbles.
    func openBubble(_ msg: Msg) {
        chats[currentChat].selected = msg.id
        chats[currentChat].startNewGame = false
        staged = nil; stagedPreview = nil
        presentation = .expanded
        drawerDismissed = false
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
        chats = [ChatState(), ChatState()]
        localIndex = 0
        currentChat = 0
        presentation = .expanded
        drawerDismissed = false
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
        // Discard the current player's half-staged move — both the payload AND the
        // pending ledger. Clearing only `staged` left the ledger behind, so a later
        // switch back replayed the stale move (Rule R) onto whatever had been
        // delivered meanwhile, forking the chain a round ahead → a bogus "this game
        // has moved on". A real device never switches identity, so this only bites
        // the harness; clear it before rebinding to the next player's suite.
        MessageGameStore.shared.clearAllPending()
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
        currentChat = idx
        staged = nil; stagedPreview = nil
        presentation = .expanded
        drawerDismissed = false
    }

    /// The current player tapped New game. New game always opens full-screen.
    func newGame() {
        chats[currentChat].startNewGame = true; staged = nil; stagedPreview = nil
        presentation = .expanded; drawerDismissed = false
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
    func stage(_ payload: Data, seat: Int) async {
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
            try? await Task.sleep(nanoseconds: 900_000_000)
            guard let self, self.staged != nil else { return }
            // Skip the collapse during an auto-played game so our move's animation
            // finishes on the full board (it's the point of the slow run); real
            // interactive staging still collapses to Messages' Send.
            if ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] == nil {
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
                try? await Task.sleep(nanoseconds: 1_400_000_000)
                // …then wait for the WHOLE bout-end sequence (discard/pickup + each
                // player's draw, one at a time) to finish before switching users, so
                // the beat reads: open → their move replays → we act → our move +
                // the full discard/draw cascade animate → switch.
                while BoardAnimator.isSequencing { try? await Task.sleep(nanoseconds: 150_000_000) }
                try? await Task.sleep(nanoseconds: 600_000_000)   // rest so the settled board reads
                guard let self, self.staged != nil else { return }
                self.deliver()
                self.become((self.localIndex + 1) % self.participants.count)
            }
        }
    }

    /// The blue send arrow: deliver the staged bubble into the shared transcript
    /// so the next participant can read it.
    func deliver() {
        guard let payload = staged else { return }
        chats[currentChat].transcript.append(Msg(url: MessageEnvelope.link(payload: payload),
                                                 senderId: localId, senderName: localName,
                                                 preview: stagedPreview))
        staged = nil
        stagedPreview = nil
        chats[currentChat].selected = nil     // the newest bubble is the selection again
        // Now that a real bubble exists, leave the new-game screen: the reload
        // this delivery triggers (transcript.count changed) must read the bubble,
        // not route back to setup. (Was done in stage(); see the note there.)
        chats[currentChat].startNewGame = false
        // Commit the sent move (the harness's didStartSending): drop it from the
        // pending ledger. Otherwise the reload re-adopts our OWN just-sent chain,
        // Rule R replays the move the chain already contains, the kernel rejects
        // the already-applied action, and adopt() falsely toasts "your move was
        // superseded". Cleared synchronously so it's gone before the reload adopts.
        MessageGameStore.shared.clearAllPending()
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
