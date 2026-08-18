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
        // Dev convenience: a fresh sim has no App Group nickname, so every board
        // scenario would otherwise stall on the NicknameGate. Seed one when empty
        // (scenarios that specifically test the gate set/clear it themselves after
        // this). Harness-only.
        if MessageGameStore.shared.nickname.isEmpty { MessageGameStore.shared.nickname = localName }
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

        case "staged-compact":
            // Round-8 #3 (the send reminder): a move STAGED but not sent,
            // sitting in the compact drawer. After the 3-second fuse the blue
            // arrow + caption fade in under the Send button. Built the way the
            // real thing is: the viewer's first legal move applied + resealed +
            // staged (Send circle lit), with the pending-ledger row that makes
            // the reloading board pick the move back up as staged (Undo shown).
            await seedDemoGame()
            await stageMoveForReview()

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

        // ---- pass 2: states the first sweep never reached --------------------

        case "take":
            // The defender is buried: attackers throw and nobody covers, so the
            // only live buttons are Cover / Take. Worst case for the table's
            // layout AND for "what do I do now" legibility.
            await dealDriven(players: playersEnv, only: [.attack], steps: 12, viewAs: .defender)

        case "endgame":
            // Deck exhausted, no trump card left to reveal, hands short. The
            // deck well and the flipped-trump slot both have to say something
            // sensible when there is nothing in them.
            await dealDriven(players: playersEnv, only: nil, steps: 400, viewAs: .actor,
                             stopWhenDeckEmpty: true)

        case "lobby-name":
            // ONE hostile name at a time (HARNESS_NAME), because the four-at-once
            // `lobby-longnames` case fails to seal and falls silently through to
            // the New-game screen - this narrows which name does it.
            let hostile = ProcessInfo.processInfo.environment["HARNESS_NAME"] ?? ""
            await makeLobby(joins: [(0, "Alex"), (1, hostile)])

        case "bighand":
            // Durak punishes: a defender who takes twice can be holding 15-20
            // cards. Does the fan still fit on a phone, and can you tell the
            // cards apart?
            await dealDriven(players: playersEnv, only: [.attack, .pickup], steps: 9,
                             viewAs: .biggestHand)

        case "spectator":
            // A group thread where only 2 of the 8 people are in the game. The
            // other 6 open the bubble and are not players.
            await makeLobby(joins: [(0, "Alex"), (1, "Vera")])
            become(min(5, participants.count - 1))

        case "oldbubble":
            // Two bubbles in the thread, and the reviewer taps the FIRST one -
            // the lobby - after the game has already been dealt. Real people do
            // this constantly; Messages keeps every bubble tappable forever.
            // Two lobby bubbles in the thread (someone joined after the first),
            // then open the OLDER one. `dealLive` cannot be used here - it calls
            // `setCount`, which rebuilds the participants and wipes the
            // transcript, so the "older" bubble would not exist.
            await makeLobby(joins: [(0, "Alex"), (1, "Vera")])
            await makeLobby(joins: [(0, "Alex"), (1, "Vera"), (2, "Boris"), (3, "Dima")])
            become(0)
            MessageGameStore.shared.nickname = "Alex"
            if let first = transcript.first { openBubble(first) }
            expand()

        case "chatswitch":
            // Seed a game in Chat A, then jump to Chat B, which has nothing.
            await seedDemoGame()
            switchChat(1)

        case "lobby-partial":
            // 3 of 8. The invite affordance and the "waiting" copy have to carry
            // this state, which is where a group game actually sits most of the time.
            await makeLobby(joins: [(0, "Alex"), (1, "Vera"), (2, "Boris")])

        case "seatpick-8":
            await dealLive(players: 8)
            becomeUnnamed(6)

        case "damaged-truncated":
            // A link that got cut by a copy/paste or a link preview - valid
            // prefix, wrong length.
            deliverRaw(URL(string: "https://foolish.cards/m#AEBAGBAF")!)

        case "foreign-scheme":
            // Somebody sends a foolish.cards link that is not a game at all.
            deliverRaw(URL(string: "https://foolish.cards/about")!)

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

    /// Play the viewer's first legal move through the model's own staging path,
    /// leaving the harness exactly where the real extension leaves a human who
    /// just played: move applied + resealed + staged (Send lit), auto-collapse
    /// scheduled, NOT sent. The pending-ledger row is what the board's Rule R
    /// rebase turns back into a staged move (canSend/Undo) when the surface
    /// reloads off the tapped (pre-move) bubble.
    private func stageMoveForReview() async {
        guard let latest,
              let bytes = try? MessageEnvelope.payloadBytes(url: latest.url),
              let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1),
              let gid = UInt64(env.gameId) else { return }
        let seat = localIndex
        let legal = await MessageKernel.shared.residentLegal(seat: seat)
        guard let m = legal.first(where: { $0.type != .wait }) else { return }
        try? await MessageKernel.shared.apply(seat: seat, move: m)
        guard let stagedPayload = try? await MessageKernel.shared.seal(
            phase: 2, lastActorSeat: seat, gameId: gid,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: env.joins) else { return }
        // Let the FIRST board (the one seedDemoGame put up) finish `begin()`
        // before planting the ledger row: begin mirrors its (empty) staged list
        // into the ledger, so a row written while it is still starting up gets
        // wiped before the reload below can rebase it.
        try? await Task.sleep(nanoseconds: 800_000_000)
        MessageGameStore.shared.setSeat(gameId: env.gameId, chatKey: chatKey, seat: seat)
        MessageGameStore.shared.setPending(
            [PendingAction(seat: seat, round: env.round, move: m)], gameId: env.gameId)
        // Reload the surface with the ledger in place, then stage - the model's
        // own collapse-after-settle takes it to the compact drawer.
        openBubble(latest)
        await stage(stagedPayload, seat: seat)
    }

    // MARK: - pass 2 building blocks

    /// How many pretend participants the launch asked for (HARNESS_PLAYERS),
    /// clamped to what the game supports.
    private var playersEnv: Int { max(2, min(8, participants.count)) }

    private enum ViewAs { case defender, actor, biggestHand }

    /// Deal a real game and drive it forward with kernel moves only, so a
    /// screenshot can land on a mid-game position that no amount of tapping
    /// would reach repeatably.
    ///
    /// - `only`: restrict which move types may be applied (nil = first legal
    ///   non-wait, which plays the game out).
    /// - `stopWhenDeckEmpty`: halt as soon as the talon runs dry, which is the
    ///   position the deck well and the trump slot have to survive.
    /// - `viewAs`: whose eyes the screenshot is taken through.
    private func dealDriven(players n: Int, only: [MoveType]?, steps: Int,
                            viewAs: ViewAs, stopWhenDeckEmpty: Bool = false) async {
        setCount(n)
        let seed = Data(repeating: 42, count: 32)
        let joins = (0..<n).map { MessageJoin(seat: $0, name: Self.nameFor($0)) }
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: n)
            var lastSeat = 0
            for _ in 0..<steps {
                if stopWhenDeckEmpty,
                   let v = await MessageKernel.shared.residentView(viewer: -1),
                   v.deckCount == 0 || v.isOver { break }
                var acted = false
                for s in 0..<n {
                    let legal = await MessageKernel.shared.residentLegal(seat: s)
                    let pick = legal.first { m in
                        guard m.type != .wait else { return false }
                        guard let only else { return true }
                        return only.contains(m.type)
                    }
                    if let m = pick {
                        try? await MessageKernel.shared.apply(seat: s, move: m)
                        lastSeat = s; acted = true; break
                    }
                }
                if !acted { break }
            }
            let view = await MessageKernel.shared.residentView(viewer: -1)
            let payload = try await MessageKernel.shared.seal(
                phase: (view?.isOver == true) ? 3 : 2, lastActorSeat: lastSeat,
                gameId: 0xF00D, parent8: Data(repeating: 0, count: 8), joins: joins)
            _ = try? await MessageKernel.shared.decode(payload: payload, viewer: -1)

            var seat = lastSeat
            switch viewAs {
            case .defender:
                if let d = view?.defender, d >= 0, d < n { seat = d }
            case .actor:
                for s in 0..<n where (await MessageKernel.shared.residentLegal(seat: s))
                    .contains(where: { $0.type != .wait }) { seat = s; break }
            case .biggestHand:
                if let players = view?.players {
                    var best = (seat: 0, n: -1)
                    for (i, p) in players.enumerated() where p.handCount > best.n {
                        best = (i, p.handCount)
                    }
                    seat = best.seat
                }
            }
            await deliverSealed(payload, senderSeat: lastSeat)
            // Seat the viewer explicitly - this rig is about the BOARD, not
            // about re-testing seat inference (which `seatpick` covers). The
            // store has to be written AFTER `become` rebinds to that
            // participant's own suite, then `become` again so the view reloads
            // with the cache in place.
            become(seat)
            MessageGameStore.shared.nickname = Self.nameFor(seat)
            if let env = try? await MessageEnvelope.decode(payload: payload, viewer: seat) {
                MessageGameStore.shared.put(MessageGameRecord(
                    gameId: env.gameId, chatKey: chatKey, mySeat: seat, nPlayers: env.nPlayers,
                    round: env.round, turn: env.turn, phase: env.phase, finished: false,
                    names: Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a }),
                    payloadBase32: Base32.encode(payload), updatedAt: 1))
            }
            become(seat)
            expand()
        } catch {}
    }
}
