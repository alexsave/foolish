// HarnessScenario — put the harness into ONE named state and stop, so a state
// can be screenshotted without driving the simulator's UI.
//
// NOT SHIPPING CODE (see FoolishHarnessApp.swift). This is the review rig: an
// App Store reviewer's pass has to reach the setup screen, a one-join lobby, a
// full lobby, a name gate, a seat picker, a damaged link, an end screen and a
// dismissed drawer — and synthetic taps (cliclick) proved too unreliable to get
// there repeatably. Each scenario below drives the SAME model API the real
// chrome's buttons drive (`newGame`, `stage`, `deliver`, `become`, ...), so what
// it lands on is a state the app can genuinely be in, not a mocked-up view.
//
// Usage: `SIMCTL_CHILD_HARNESS_SCENARIO=<name>` on `simctl launch`. Unknown
// names are a no-op (the plain New-game screen).

import SwiftUI
import UIKit
import FoolishKit

extension HarnessModel {

    static var scenarioName: String? { ProcessInfo.processInfo.environment["HARNESS_SCENARIO"] }

    /// The one entry point. Runs to a stable state and returns; nothing here
    /// loops or schedules follow-up work, so the screenshot after it is settled.
    func runScenario(_ name: String) async {
        switch name {

        // ---- setup / naming ------------------------------------------------
        case "setup":
            newGame()

        case "setup-longname":
            // A stored nickname is what pre-fills the setup field. 64 chars of
            // mixed script + emoji: does the field clip, wrap, or push the
            // Create button off the surface?
            MessageGameStore.shared.nickname = "Bartholomew Aloysius Featherstonehaugh-Смирнов 🐴🎩🃏🂡🂢"
            newGame()

        // ---- lobby ---------------------------------------------------------
        case "lobby-mine":                    // I created it, nobody else joined
            await makeLobby(joins: [(0, localName)])

        case "lobby-received":                // somebody else's invite, I have not joined
            await makeLobby(joins: [(0, "Alex")])
            become(1)

        case "lobby-half":                    // 4 of 8 in
            await makeLobby(joins: [(0, "Alex"), (1, "Vera"), (2, "Boris"), (3, "Dima")])

        case "lobby-full":                    // all 8 in — Start should be offered
            await makeLobby(joins: (0..<8).map { ($0, Self.nameFor($0)) })

        case "lobby-longnames":               // wire/layout stress on the roster
            await makeLobby(joins: [
                (0, "Bartholomew Aloysius Featherstonehaugh-Смирнов"),
                (1, "🃏🂡🂢🂣🂤🂥🂦🂧🂨🂩🂪🂫🂭🂮"),
                (2, "                    "),          // whitespace-only
                (3, "<script>alert(1)</script>"),     // markup, in case anything renders rich text
            ])

        // ---- boards --------------------------------------------------------
        case "board":
            await seedDemoGame()

        case "board-compact":
            await seedDemoGame()
            collapseForReview()

        // ---- identity edges -------------------------------------------------
        case "namegate":
            // A DM receiver who has never named themselves: the creator named
            // themselves in setup, the opponent has neither a name nor a cache.
            await seedDemoGame()
            becomeUnnamed(1)

        case "seatpick":
            // 4 players, and I am a non-sender with no cached seat — §6.3
            // ambiguity. DEBUG offers the picker; Release shows a spectator board.
            await dealLive(players: 4)
            becomeUnnamed(2)

        // ---- failure surfaces -----------------------------------------------
        case "damaged":
            deliverRaw(URL(string: "https://foolish.cards/m#AAAAAAAABBBBBBBB")!)

        case "damaged-empty":
            deliverRaw(URL(string: "https://foolish.cards/m")!)

        case "dismissed":
            newGame()
            dismissDrawer()

        default:
            break
        }
    }

    // MARK: - building blocks

    static func nameFor(_ seat: Int) -> String {
        ["Alex", "Vera", "Boris", "Dima", "Katya", "Lev", "Mila", "Oleg"][seat % 8]
    }

    /// Seal + deliver a WAITING (phase 0) lobby with exactly `joins` seated, the
    /// way `createWaiting`/`joinLobby` would have after that many taps. Capacity
    /// is the group-chat 8 unless the harness is a DM.
    private func makeLobby(joins: [(Int, String)]) async {
        let capacity = chatIsDM ? 2 : 8
        let seed = Data(repeating: 7, count: 32)
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: joins.map(\.0).max() ?? 0, gameId: 0xB0BB,
                parent8: Data(repeating: 0, count: 8),
                joins: joins.map { MessageJoin(seat: $0.0, name: $0.1) })
            MessageGameStore.shared.nickname = localName
            await deliverSealed(payload, senderSeat: joins.map(\.0).max() ?? 0)
        } catch {
            // fall through to the New-game screen — a failure here is itself a note
        }
    }

    /// Deal a real N-player game and deliver the LIVE handoff bubble from seat 0.
    private func dealLive(players n: Int) async {
        setCount(n)
        let seed = Data(repeating: 42, count: 32)
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: n)
            let joins = (0..<n).map { MessageJoin(seat: $0, name: Self.nameFor($0)) }
            let payload = try await MessageKernel.shared.seal(
                phase: 2, lastActorSeat: 0, gameId: 0xF00D,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            await deliverSealed(payload, senderSeat: 0)
        } catch {}
    }

    /// Append a sealed payload to the open transcript as if `senderSeat` sent it,
    /// then view it as that seat (the sender's own device).
    private func deliverSealed(_ payload: Data, senderSeat: Int) async {
        // Be the seat that acted BEFORE staging, or the harness attributes the
        // bubble to whoever is currently "you" while the envelope names another
        // seat as the actor — and §6 identity then resolves the viewer as that
        // other seat ("you are: Alex" over a lobby reading "8. Oleg (You)").
        if senderSeat != localIndex, senderSeat < participants.count { become(senderSeat) }
        await stage(payload, seat: senderSeat)
        // `stage` schedules a collapse; the review wants the expanded board, and
        // deliver() below is the human's Send.
        deliver()
        expand()
    }

    /// Append a bubble carrying a URL we did not seal — the corrupt-link path.
    private func deliverRaw(_ url: URL) {
        appendForeignBubble(url: url)
        expand()
    }

    /// Become `idx` WITHOUT the pre-naming `become` normally leaves alone — i.e.
    /// a genuinely fresh device: empty store, no nickname, no cached seat.
    private func becomeUnnamed(_ idx: Int) {
        become(idx)
        MessageGameStore.shared.nickname = ""
    }

    private func collapseForReview() { togglePresentation() }
}
