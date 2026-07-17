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

import SwiftUI
import FoolishKit

@MainActor
final class HarnessModel: ObservableObject {
    struct Participant: Identifiable, Equatable { let id = UUID(); let name: String }
    struct Msg: Identifiable, Equatable { let id = UUID(); let url: URL; let senderId: UUID; let senderName: String }

    @Published private(set) var participants: [Participant]
    @Published private(set) var localIndex = 0
    @Published private(set) var transcript: [Msg] = []
    /// True when the current player is starting a fresh game (routes to setup)
    /// rather than reading the latest transcript bubble.
    @Published private(set) var startNewGame = true
    /// The bubble the board auto-staged (the extension's `insert`), not yet
    /// delivered. Stands in for the Messages input field: the harness Send button
    /// is the blue send arrow. nil = nothing staged.
    @Published private(set) var staged: Data?
    /// DEV diagnostic surfaced in the chrome (seat/turn/legal after a seed).
    @Published private(set) var debugInfo = ""

    private static let names = ["You", "Vera", "Boris", "Dima", "Katya", "Lev", "Mila", "Oleg"]

    init(count: Int = 2) {
        participants = Self.make(count)
        rebindStore()
    }

    private static func make(_ n: Int) -> [Participant] {
        (0..<max(2, min(8, n))).map { Participant(name: names[$0]) }
    }

    // MARK: derived inputs for MessagesRootView

    var localId: UUID { participants[localIndex].id }
    var localName: String { participants[localIndex].name }
    var playerCount: Int { participants.count }
    var latest: Msg? { transcript.last }
    var payloadURL: URL? { startNewGame ? nil : latest?.url }
    /// Did the CURRENT player send the latest bubble? Drives §6.2 sender inference.
    var senderIsLocal: Bool { latest?.senderId == localId }
    var chatIsDM: Bool { participants.count == 2 }
    /// Reset MessagesRootView's @State whenever the player, the transcript, or the
    /// new-game intent changes, so it re-derives as the current participant.
    var viewKey: String { "\(localIndex)-\(transcript.count)-\(startNewGame)" }

    // MARK: actions

    /// Change the number of pretend participants and start over.
    func setCount(_ n: Int) {
        participants = Self.make(n)
        transcript = []
        localIndex = 0
        startNewGame = true
        rebindStore()
    }

    /// "Become" participant `idx` — the crux of the harness. Rebinds the seat
    /// cache to that participant's own suite and reads the latest transcript
    /// bubble as them (senderIsLocal recomputes, so they are the receiver).
    func become(_ idx: Int) {
        guard idx >= 0, idx < participants.count else { return }
        localIndex = idx
        startNewGame = false
        staged = nil                 // a half-staged move doesn't cross to another player
        rebindStore()
    }

    /// The current player tapped New game.
    func newGame() { startNewGame = true; staged = nil }

    /// The board auto-staged a chain (the extension's `insert`). Hold it; the
    /// human still has to press Send — that is `deliver()`.
    func stage(_ payload: Data, seat: Int) {
        staged = payload
        startNewGame = false
    }

    /// The blue send arrow: deliver the staged bubble into the shared transcript
    /// so the next participant can read it.
    func deliver() {
        guard let payload = staged else { return }
        transcript.append(Msg(url: MessageEnvelope.link(payload: payload),
                              senderId: localId, senderName: localName))
        staged = nil
    }

    // Each participant → its own throwaway cache suite (fresh per app launch via
    // the per-run UUIDs), so no seat leaks between "devices".
    private func rebindStore() {
        MessageGameStore.shared = MessageGameStore(suiteName: "fmsg.harness.\(localId.uuidString)")
    }

    /// DEV screenshotting: deal a real 2-player game, play seat 0's opening
    /// attack, and land as the receiver (Vera) viewing that bubble — so the board
    /// renders without any taps. Gated by the HARNESS_SEED launch env; never runs
    /// in normal use.
    func seedDemoGame() async {
        guard participants.count >= 2 else { return }
        let seed = Data(repeating: 42, count: 32)   // fixed → reproducible screenshots
        let gid: UInt64 = 0xF001
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: 2)
            let legal = await MessageKernel.shared.residentLegal(seat: 0)
            if let atk = legal.first(where: { $0.type == .attack }) {
                try await MessageKernel.shared.apply(seat: 0, move: atk)
            }
            let payload = try await MessageKernel.shared.seal(
                phase: 2, lastActorSeat: 0, gameId: gid,
                parent8: Data(repeating: 0, count: 8),
                joins: [MessageJoin(seat: 0, name: participants[0].name)])
            // Diagnostic: after decoding the sealed chain, what can each seat do?
            _ = try? await MessageKernel.shared.decode(payload: payload, viewer: 1)
            let l0 = await MessageKernel.shared.residentLegal(seat: 0)
            let l1 = await MessageKernel.shared.residentLegal(seat: 1)
            let v = await MessageKernel.shared.residentView(viewer: 1)
            debugInfo = "def=\(v?.defender ?? -9) s0=[\(l0.map { "\($0.type)" }.joined(separator: ","))] s1=[\(l1.map { "\($0.type)" }.joined(separator: ","))]"

            transcript = [Msg(url: MessageEnvelope.link(payload: payload),
                              senderId: participants[0].id, senderName: participants[0].name)]
            localIndex = 1           // become the receiver → the board shows
            startNewGame = false
            rebindStore()
        } catch {
            // leave the harness on the New-game screen if seeding fails
        }
    }
}
