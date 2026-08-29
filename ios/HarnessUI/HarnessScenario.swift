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

        case "board-sorted":
            // Round-8 #4: the persisted hand arrangement, seed-to-render. Store
            // a REVERSED arrangement for the demo game, then reopen the bubble:
            // the board must deal the same kernel hand but render it reversed.
            // Compare against the plain `board` scenario to see the difference.
            await seedDemoGame()
            await sortHandForReview()

        case "staged-compact":
            // Round-8 #3 (the send reminder): a move STAGED but not sent,
            // sitting in the compact drawer. After the 3-second fuse the blue
            // arrow + caption fade in under the Send button. ROUND 9: the
            // pending-ledger trick this used to seed the staged state with is
            // gone with the ledger itself - pair this scenario with
            // SIMCTL_CHILD_HARNESS_AUTOMOVE=1 instead: the board auto-plays the
            // first human-playable move through its OWN tap path, which stages
            // and auto-collapses exactly like a real player.
            await seedDemoGame()

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

        case "arrival":
            // A move arriving on a board that is already open (round 17).
            await arrivalOnOpenBoard(players: playersEnv)

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

        case "spectator-over":
            // ROUND 20: the RELEASE spectator screen with a FINISHED game on it -
            // the one surface no rig could reach (it is `#else` to a `#if DEBUG`,
            // and every harness build is a debug build), which is how it came to
            // show a swept empty table and "open the game from your own bubble to
            // play" to somebody looking at a game that was over.
            MessageDebugFlags.spectateWhenAmbiguous = true
            await finishedGameWatchedByAnOnlooker(players: playersEnv)

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
    /// ROUND 20: A GAME PLAYED OUT TO THE END, OPENED BY SOMEBODY WHO IS NOT IN IT.
    ///
    /// Not `dealDriven`, for two reasons it cannot give: the chat has to hold
    /// MORE PEOPLE THAN SEATS (`setCount` is the only way to say so, and it
    /// wipes the transcript, so it has to come first), and the game has to
    /// genuinely REACH game over rather than run a fixed number of steps. It
    /// covers by preference to get there, the same trick the offline endgame
    /// finder uses - rounds of pickups never finish.
    private func finishedGameWatchedByAnOnlooker(players n: Int) async {
        setCount(n + 1)                       // …one more person than seats
        let seed = Data(repeating: 42, count: 32)
        let joins = (0..<n).map { MessageJoin(seat: $0, name: Self.nameFor($0)) }
        guard (try? await MessageKernel.shared.newGame(seed: seed, players: n)) != nil
        else { return }
        var lastSeat = 0
        for _ in 0..<800 {
            guard let v = await MessageKernel.shared.residentView(viewer: -1), !v.isOver
            else { break }
            var acted = false
            for s in 0..<n {
                let legal = await MessageKernel.shared.residentLegal(seat: s)
                if let m = legal.first(where: { $0.type == .cover })
                    ?? legal.first(where: { $0.type != .wait }) {
                    try? await MessageKernel.shared.apply(seat: s, move: m)
                    lastSeat = s; acted = true; break
                }
            }
            if !acted { break }
        }
        let over = (await MessageKernel.shared.residentView(viewer: -1))?.isOver == true
        AnimLog.say("scenario: spectator-over - game \(over ? "finished" : "DID NOT FINISH - rig bug")")
        guard let payload = try? await MessageKernel.shared.seal(
                phase: over ? 3 : 2, lastActorSeat: lastSeat, gameId: 0xF00D,
                parent8: Data(repeating: 0, count: 8), joins: joins)
        else { return }
        await deliverSealed(payload, senderSeat: lastSeat)
        // …and now be the ONLOOKER: the participant past the last seat, with
        // their own empty seat cache and a name that appears in no join. That is
        // `.ambiguous` (§6.2/§6.3). NOT `becomeUnnamed` - an unnamed device is
        // sent to the NAME GATE before identity is weighed at all, which is what
        // the obvious version of this scenario landed on.
        become(n)
        MessageGameStore.shared.nickname = Self.nameFor(n)
        become(n)
        expand()
    }

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

    /// Round-8 #4: store a reversed arrangement for the viewer's demo hand, the
    /// way a real reorder would have persisted it, then reload the surface so
    /// the board seeds from it - proving the store -> FHandFan seed leg end to
    /// end (the reorder -> store leg is the fan's onOrderChanged, gesture-only).
    private func sortHandForReview() async {
        guard let latest,
              let bytes = try? MessageEnvelope.payloadBytes(url: latest.url),
              let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1),
              let hand = await MessageKernel.shared.residentView(viewer: localIndex)?.me?.hand
        else { return }
        MessageGameStore.shared.setHandOrder(hand.reversed().map(\.identity), gameId: env.gameId)
        openBubble(latest)
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

    /// A MOVE ARRIVES ON THE OPEN BOARD - the lifecycle nothing here could pose
    /// until `HarnessModel.arrive` existed, and the one the owner's report is
    /// about ("the attack comes in, it animates that card moving, but it just
    /// doesn't land on the table - the card just vanishes").
    ///
    /// Built in two halves on purpose. The first is an ordinary chain, opened
    /// the way any board is opened, so the board is warm and measured before
    /// anything arrives - a cold mount is the case that already worked. The
    /// second plays ONE further move from another seat against the resident
    /// kernel, seals it, and hands it to the live surface as an arrival.
    ///
    /// `HARNESS_ARRIVE_KIND` picks what arrives:
    ///  - `attack` (default): the smallest thing that can go wrong - one card,
    ///    one new battle.
    ///  - `cover`: the receiver watches a cover land on an attack (the owner's
    ///    "shows the card moving... then just vanishes and the attack card it
    ///    covered rotates back to 0 degrees").
    ///  - `pickup`: the arriving move ends the bout by taking the table - a
    ///    round transition with a sweep and refills.
    ///  - `goodend`: the arriving move is the closing GOOD over a fully covered
    ///    table - discard sweep, refills, role hand-off, the longest stream an
    ///    arrival can interrupt (run with HARNESS_PLAYERS=2 so one good closes).
    ///  - `coverend` (round 20): the arriving move is the COVER THAT CLOSES ITS
    ///    OWN BOUT - the defender's last cards go down and the table goes with
    ///    them in one apply. The owner's "on the last cover for a set, you need
    ///    to show the cover animation, then pause then sweep. I wasn't seeing
    ///    the cover animation on a replay." It is the only kind that has to be
    ///    played DEEP into a game to reach (see the warmup below), because the
    ///    kernel closes a bout on a cover for exactly one reason: the defender
    ///    has run out of cards.
    /// The kinds beyond `attack` exist because the first cut of this scenario
    /// bailed out silently when the fixed six-step warmup happened not to leave
    /// the wanted move legal - so the cover and round-transition arrivals (the
    /// owner's symptoms 3 and 4) were never actually posed. The warmup now
    /// DRIVES until the wanted move is available instead of hoping.
    private func arrivalOnOpenBoard(players n: Int) async {
        setCount(n)
        let seed = Data(repeating: 42, count: 32)
        let joins = (0..<n).map { MessageJoin(seat: $0, name: Self.nameFor($0)) }
        guard (try? await MessageKernel.shared.newGame(seed: seed, players: n)) != nil
        else { return }

        let kind = ProcessInfo.processInfo.environment["HARNESS_ARRIVE_KIND"] ?? "attack"
        let want: MoveType = switch kind {
        case "cover", "coverend", "gameover": .cover
        case "pickup": .pickup
        case "goodend": .good
        default: .attack
        }
        // `gameover` is `coverend` without the survive-the-bout condition: the
        // defender's last cards go down on an EMPTY deck at two seats, so the
        // same move that closes the bout closes the game. Owner, round 20:
        // "replaying a final move doesn't even show the winning move animation.
        // It should show the final move animation, then the ranks."
        let deep = kind == "coverend" || kind == "gameover"
        // Is the wanted ARRIVAL playable right now? For `goodend` the good must
        // actually CLOSE the bout, which means a non-empty, fully covered table
        // - a good over open attacks merely passes priority and animates
        // nothing, which is not the arrival being posed.
        func wantReady() async -> Int? {
            guard let v = await MessageKernel.shared.residentView(viewer: -1) else { return nil }
            if kind == "goodend" {
                guard !v.battles.isEmpty, v.battles.allSatisfy({ $0.defense != nil })
                else { return nil }
            }
            for s in 0..<n {
                let legal = await MessageKernel.shared.residentLegal(seat: s)
                // ROUND 20, `coverend`: the cover must CLOSE the bout, which the
                // kernel decides on exactly one condition - the defender's hand
                // is empty afterwards (game.c handle_cover, `def->hand_count ==
                // 0`). So the move being waited for is a cover that spends the
                // defender's LAST cards, and any other cover is merely a cover.
                if deep {
                    // …and the DECK must still hold something. The same branch
                    // that discards the table refills the hands right after it
                    // (game.c `refill_player_hands`), so with an empty deck the
                    // defender simply goes out - which in a 2p game is the game
                    // ending, not a bout ending, and seals as a FINISHED chain
                    // the live surface refuses to decode as a board. The move
                    // being posed is a round transition, so it needs a round to
                    // transition into.
                    //
                    // With an empty deck it still works at 3+ seats, where one
                    // player going out leaves a game to carry on - which is the
                    // commoner shape by far, since a defender only runs out at
                    // all once the deck has.
                    let held = v.players.first { $0.seat == s }?.handCount ?? -1
                    let stillIn = v.players.filter { !$0.isOut }.count
                    let survives = v.deckCount > 0 || stillIn > 2
                    if kind == "coverend" ? survives : true,
                       legal.contains(where: { $0.type == .cover && $0.cards.count == held }) { return s }
                    continue
                }
                if legal.contains(where: { $0.type == want }) { return s }
            }
            return nil
        }
        // Warm the game up to a real mid-game board (at least 4 moves), then
        // keep stepping until the wanted arrival is on somebody's menu. The cap
        // only guards a kind this seed can never produce; every kind above
        // shows up within a bout or two.
        //
        // HARNESS_ARRIVE_WARMUP=0 is the FIRST-MOVE case (two devices, 1.0(19)):
        // the opened chain is the untouched deal, zero goods ever played, and
        // the arrival is the game's very first attack landing on the open warm
        // board. Default 4 keeps every existing run identical.
        let minWarmup = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_WARMUP"] ?? "4") ?? 4
        var lastSeat = 0
        var steps = 0
        // ROUND 20: a bout-ending COVER is an ENDGAME shape - it needs a
        // defender down to their last cards - so `coverend` drives far deeper
        // than the other kinds, and PREFERS covering all the way there (the same
        // two tricks MessageBoutEndHoldTests.findClosingCover uses to reach the
        // same board offline: round after round of pickups never gets there).
        let cap = deep ? 400 : 40
        // …and if one whole game goes by without producing the board, RE-DEAL.
        // Not every deal contains a bout-ending cover at all (the defender has
        // to run out on a table they can fully answer), so a single game is a
        // coin toss - the offline finder loops 40 seeds for the same reason. Any
        // other kind takes exactly one pass, as it always has.
        let deals = deep ? 40 : 1
        deal: for salt in 0..<deals {
            if salt > 0 {
                let reseed = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ salt) | 1 })
                guard (try? await MessageKernel.shared.newGame(seed: reseed, players: n)) != nil
                else { break }
                steps = 0
            }
            while steps < cap {
                if steps >= minWarmup, await wantReady() != nil { break deal }
                var acted = false
                for s in 0..<n {
                    let legal = await MessageKernel.shared.residentLegal(seat: s)
                    let pick = deep
                        ? (legal.first { $0.type == .cover } ?? legal.first { $0.type != .wait })
                        : legal.first { $0.type != .wait }
                    if let m = pick {
                        try? await MessageKernel.shared.apply(seat: s, move: m)
                        lastSeat = s; acted = true; break
                    }
                }
                if !acted { break }
                steps += 1
            }
        }
        guard let view = await MessageKernel.shared.residentView(viewer: -1),
              let opened = try? await MessageKernel.shared.seal(
                phase: 2, lastActorSeat: lastSeat, gameId: 0xF00D,
                parent8: Data(repeating: 0, count: 8), joins: joins)
        else { return }
        _ = try? await MessageKernel.shared.decode(payload: opened, viewer: -1)

        // Watch as somebody who is NOT about to move, so what arrives is
        // unambiguously somebody else's move landing on my open board.
        guard let mover = await wantReady() else {
            AnimLog.say("scenario: no seat can play \(kind) after \(steps) steps - rig bug")
            return
        }
        let watcher = (0..<n).first { $0 != mover && $0 != view.defender } ?? ((mover + 1) % n)

        // ROUND 21: HARNESS_ARRIVE_COLD=1 poses the OTHER half of the same
        // move - the bubble opened from the transcript rather than landing on a
        // board that is already up. The owner draws the distinction himself:
        // "if we just played good and we send it off, we shouldn't show the good
        // animation… but if we close and open to REPLAY it, then for sure we
        // should show our own good animation." A live arrival keeps its role
        // marks in `@State` across the move; a cold open has to be told where
        // they stood, which is the entire difference being tested.
        //
        // Mechanically it is this same rig minus the pre-mount: the parent
        // chain is never delivered, so the first thing the surface ever sees is
        // the bubble carrying the move.
        let cold = ProcessInfo.processInfo.environment["HARNESS_ARRIVE_COLD"] == "1"
        if !cold {
            await deliverSealed(opened, senderSeat: lastSeat)
        }
        become(watcher)
        MessageGameStore.shared.nickname = Self.nameFor(watcher)
        become(watcher)
        expand()
        // Let the board mount, decode and settle - the arrival must land on a
        // board that is already up, which is the whole point. A cold open has
        // nothing to mount yet, so it waits only long enough for the surface
        // itself to be there.
        try? await Task.sleep(nanoseconds: cold ? 400_000_000 : 2_500_000_000)

        // …now the other seats play, and each ARRIVES. `HARNESS_ARRIVE_N` (with
        // `HARNESS_ARRIVE_GAP` in milliseconds) is the case the owner reports:
        // bubbles landing one after another on a board that is still animating
        // the last one, which is what a real table does when two people are
        // playing at once. A gap SHORTER than a flight is the point.
        let n_arrivals = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_N"] ?? "1") ?? 1
        let gapMs = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_GAP"] ?? "250") ?? 250
        var lastPayload = opened
        for i in 0..<max(1, n_arrivals) {
            // WAIT FOR THE LIVE CONTROLLER TO FINISH FOLDING THE PREVIOUS CHAIN
            // IN, then re-point the resident at it before composing the next
            // move. The rig shares ONE resident kernel between the "sender"
            // (this scenario) and the receiver's board, which no real thread
            // does - two phones each hold their own. Without this handshake the
            // controller's in-flight adopt (it decodes the chain up to three
            // times) re-pointed the resident AFTER this loop had applied the
            // next move, so the seal read a game the move had been WIPED from
            // and emitted a bubble carrying nothing new - a thread no real pair
            // of phones can produce, failing the oracle against a rig bug.
            // A cold open has no previous chain on the surface to wait for -
            // the handshake below is about a LIVE controller folding the last
            // arrival in, and on the first pass of a cold run there is none.
            let deadline = Date().addingTimeInterval(4)
            while Date() < deadline, !(cold && i == 0),
                  MessageTurnController.debugLatest?.basePayload != lastPayload {
                try? await Task.sleep(nanoseconds: 30_000_000)
            }
            _ = try? await MessageKernel.shared.decode(payload: lastPayload, viewer: -1)
            // The child bubble names its parent's digest, exactly as
            // MessageTurnController.stagedPayload does on a phone - Rule P's
            // rule 4 (a child outranks the parent it names) needs the link, so
            // a rig that sealed zeros would keep hitting the digest coin flip
            // the real extension no longer plays.
            let parent8 = (try? await MessageKernel.shared.peek(payload: lastPayload))
                .map { MessageTurnController.firstEight(hex: $0.digest) }
                ?? Data(repeating: 0, count: 8)
            var acted: (seat: Int, move: Move)?
            let held = await MessageKernel.shared.residentView(viewer: -1)
            for s in 0..<n where acted == nil {
                guard s != watcher else { continue }
                let legal = await MessageKernel.shared.residentLegal(seat: s)
                // The first arrival is the KIND asked for; the rest are whatever
                // that seat can legally do next, which is what a real thread
                // delivers. Round 20: `coverend` picks the cover that spends the
                // defender's LAST cards, since any other cover leaves the table
                // standing and poses a different arrival entirely.
                let mine = held?.players.first { $0.seat == s }?.handCount ?? -1
                let m = i > 0 ? legal.first { $0.type != .wait }
                    : deep
                        ? legal.first { $0.type == .cover && $0.cards.count == mine }
                        : legal.first { $0.type == want }
                if let m { acted = (s, m) }
            }
            // Nobody but the watcher can act (a 2p goodend hands the next bout
            // to the very seat that is watching): the thread has delivered all
            // it can, so stop ARRIVING - but never skip the oracle below, which
            // is the entire point of the run.
            guard let a = acted,
                  (try? await MessageKernel.shared.apply(seat: a.seat, move: a.move)) != nil
            else { break }
            // ROUND 20: a move that ENDED THE GAME seals as FINISHED, which is
            // what a real phone does - and a rig that sealed LIVE regardless
            // produced a chain the surface refuses ("arrival ignored - decode
            // failed"), so the one arrival worth watching most, the winning
            // move, could not be posed at all.
            let over = (await MessageKernel.shared.residentView(viewer: -1))?.isOver == true
            guard let next = try? await MessageKernel.shared.seal(
                    phase: over ? 3 : 2, lastActorSeat: a.seat, gameId: 0xF00D,
                    parent8: parent8, joins: joins)
            else { break }
            AnimLog.say("scenario: arrival \(i + 1) - \(Self.nameFor(a.seat)) plays \(a.move.type), watcher=\(watcher)")
            arrive(next, senderIndex: a.seat)
            // HARNESS_ARRIVE_DUP=<ms>: deliver the SAME bubble a second time
            // that many milliseconds later - a duplicate didReceive. The
            // maybeAdoptIncoming "same chain" guard reads controller.basePayload,
            // which the first adopt only updates once its detached Task runs, so
            // a tight duplicate races past it and adopts the same chain twice.
            // The second begin() re-arms replayPending, publishes an UNCHANGED
            // view, and no onChange ever consumes the veil - the landed card
            // sits laid out at opacity 0 (the owner's vanish).
            if let dupMs = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_DUP"] ?? "") {
                try? await Task.sleep(nanoseconds: UInt64(max(dupMs, 0)) * 1_000_000)
                AnimLog.say("scenario: duplicate delivery of arrival \(i + 1)")
                arrive(next, senderIndex: a.seat)
            }
            lastPayload = next
            if i + 1 < n_arrivals {
                try? await Task.sleep(nanoseconds: UInt64(gapMs) * 1_000_000)
            }
        }
        // THE ORACLE. Everything above is what the thread did; this is what the
        // board must be showing once it settles, read from the kernel that just
        // played it. A rig that only takes a screenshot cannot tell "a bit
        // behind" from "correct" without a human who remembers the moves.
        try? await Task.sleep(nanoseconds: 6_000_000_000)
        if let truth = await MessageKernel.shared.residentView(viewer: watcher) {
            AnimLog.say("TRUTH battles=\(truth.battles.count) "
                + "table=[\(truth.battles.map { b in "\(b.attack.identity)/\(b.defense?.identity ?? "-")" }.joined(separator: " "))] "
                + "deck=\(truth.deckCount) discard=\(truth.discardCount) "
                + "myhand=\(truth.me?.handCount ?? -1) "
                + "hands=[\(truth.players.map { "\($0.seat):\($0.handCount)" }.joined(separator: " "))] "
                + "def=\(truth.defender) fa=\(truth.firstAttacker)")
        }
        // THE ASSERTION. A board at rest that disagrees with the kernel is the
        // whole report; counting the paints is what turns "it seems to be a bit
        // behind" into a number that can be driven to zero.
        AnimLog.say("STALE-AT-REST paints: \(MessageTableView.staleAtRest) "
            + "BACKWARDS paints: \(MessageTableView.backwardsPaints) "
            + "VANISHED paints: \(MessageTableView.vanishedAtRest)")
        // THE CONTROLLER ORACLE. The paint counters above can only see what the
        // board happened to draw while something was still repainting; a board
        // that settles WRONG and then draws nothing (no repaints at rest)
        // slips past all three. So ask the source the board renders from: the
        // live controller's published view must EQUAL the kernel's truth, and
        // its veil (`replayPending`) must be down. `replayPending` still up
        // this long after the last arrival is a veil nothing will ever take
        // down - the board is hiding cards and freezing counts at rest, which
        // is the owner's vanished cover / stale deck, as a number.
        if let c = MessageTurnController.debugLatest {
            let shown = c.view
            let truth = await MessageKernel.shared.residentView(viewer: watcher)
            let behind: Bool = {
                guard let s = shown, let t = truth else { return true }
                return s.battles != t.battles || s.deckCount != t.deckCount
                    || s.discardCount != t.discardCount
                    || s.players.map(\.handCount) != t.players.map(\.handCount)
            }()
            // ROUND 22, and the half this rig could not see: THE BUTTONS. The
            // owner's "pickup button won't appear if attack arrives while board
            // is open" leaves the table perfectly correct - `behind` is false -
            // and only the MENU wrong, because the view and the legal moves
            // used to be read on two separate trips into a kernel that holds
            // one game. A board showing an uncovered attack against me with no
            // pickup on it is not a cosmetic fault; it is a turn I cannot take.
            let truthMenu = await MessageKernel.shared.residentLegal(seat: watcher)
            let menuBehind = Set(c.legal.map(\.type)) != Set(truthMenu.map(\.type))
            AnimLog.say("ORACLE stuckVeil=\(c.replayPending) viewBehind=\(behind) "
                + "menuBehind=\(menuBehind) "
                + "shown=[battles=\(shown?.battles.count ?? -1) deck=\(shown?.deckCount ?? -1) "
                + "discard=\(shown?.discardCount ?? -1) "
                + "hands=\(shown?.players.map(\.handCount) ?? [])] "
                + "menu=\(c.legal.map { "\($0.type)" }.sorted().joined(separator: ",")) "
                + "truthmenu=\(truthMenu.map { "\($0.type)" }.sorted().joined(separator: ","))")
            AnimLog.say("BOARD-STUCK: \((c.replayPending ? 1 : 0) + (behind ? 1 : 0) + (menuBehind ? 1 : 0))")
        } else {
            AnimLog.say("ORACLE no controller - rig bug")
        }
    }
}
