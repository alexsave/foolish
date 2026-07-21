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
import FoolishKit

@MainActor
final class HarnessModel: ObservableObject {
    struct Participant: Identifiable, Equatable { let id = UUID(); let name: String }
    struct Msg: Identifiable, Equatable { let id = UUID(); let url: URL; let senderId: UUID; let senderName: String }

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
    /// Simulated Messages presentation style. The real extension collapses to the
    /// compact drawer (and Messages' Send) once a move is staged; the harness has no
    /// Messages host, so it fakes the same expanded<->compact transition here so the
    /// collapse flow is testable. `.expanded` = full board; `.compact` = drawer.
    @Published private(set) var presentation: MsgPresentation = .expanded
    /// DEV diagnostic surfaced in the chrome (seat/turn/legal after a seed).
    @Published private(set) var debugInfo = ""

    private static let names = ["You", "Vera", "Boris", "Dima", "Katya", "Lev", "Mila", "Oleg"]

    init(count: Int? = nil) {
        let env = Int(ProcessInfo.processInfo.environment["HARNESS_PLAYERS"] ?? "")
        participants = Self.make(count ?? env ?? 2)
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
    var payloadURL: URL? { startNewGame ? nil : latest?.url }
    /// Did the CURRENT player send the latest bubble? Drives §6.2 sender inference.
    var senderIsLocal: Bool { latest?.senderId == localId }
    var chatIsDM: Bool { participants.count == 2 }
    /// Reset MessagesRootView's @State whenever the player, the chat, the
    /// transcript, or the new-game intent changes, so it re-derives as the
    /// current participant IN the current chat.
    var viewKey: String { "\(localIndex)-\(currentChat)-\(transcript.count)-\(startNewGame)" }

    // MARK: actions

    /// Change the number of pretend participants and start over (both chats).
    func setCount(_ n: Int) {
        participants = Self.make(n)
        chats = [ChatState(), ChatState()]
        localIndex = 0
        currentChat = 0
        presentation = .expanded
        rebindStore()
    }

    /// The compact drawer's "Open the game" (the extension's requestPresentationStyle).
    func expand() { presentation = .expanded }
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
        staged = nil                 // a half-staged move doesn't cross to another player
        presentation = .expanded     // opening the game as this player
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
        staged = nil
        presentation = .expanded
    }

    /// The current player tapped New game. New game always opens full-screen.
    func newGame() { chats[currentChat].startNewGame = true; staged = nil; presentation = .expanded }

    /// Retract the staged bubble (§10 undo, batch-1 note 32): an undo that empties
    /// the pending ledger must drop the Send-lit payload too, not just no-op and
    /// leave a stale move ready to send. Stands in for the real extension's
    /// `pendingStage = nil` — the harness has no inserted-bubble UI to remove.
    func unstage() { staged = nil }

    /// The board auto-staged a chain (the extension's `insert`). Hold it; the
    /// human still has to press Send — that is `deliver()`.
    ///
    /// Staging must NOT touch `startNewGame`: `viewKey` keys the live
    /// MessagesRootView on it, so flipping it here would tear down the very board
    /// that just staged the move (it reloads with payloadURL=nil → "damaged").
    /// The new-game intent only clears when the bubble is actually delivered.
    func stage(_ payload: Data, seat: Int) {
        staged = payload
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
                                                 senderId: localId, senderName: localName))
        staged = nil
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
            chats[0] = ChatState(transcript: [Msg(url: MessageEnvelope.link(payload: payload),
                                                  senderId: participants[0].id, senderName: participants[0].name)],
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
