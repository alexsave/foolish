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
        // ROUND 30: HARNESS_LANG puts the whole rig in one language, so a board,
        // a rulebook and a caption can each be LOOKED AT in it. Five languages
        // is past the point where reading the table proves anything - a string
        // can be correct and still overflow its button, and the only way to know
        // is a screenshot.
        if let code = ProcessInfo.processInfo.environment["HARNESS_LANG"],
           let lang = AppLanguage(rawValue: code) {
            FStrings.override = lang
            AnimLog.say("scenario: language = \(code)")
        }
        // …and HARNESS_TABLE the same for the table material, so both looks can
        // be screenshotted without tapping through the settings sheet.
        if let mat = ProcessInfo.processInfo.environment["HARNESS_TABLE"],
           let surface = TableSurface(rawValue: mat) {
            FPrefs.shared.setTable(surface)
            AnimLog.say("scenario: table = \(mat)")
        }
        switch name {

        // ---- setup / naming ------------------------------------------------
        case "setup":
            newGame()

        case "setup-compact":
            // ROUND 39: the New-game screen in the COMPACT DRAWER, which is
            // where the owner cannot use it. "Can't fucking got create game un
            // collapsed, only expanded. Create game. Name field. Passing
            // setting are all offset like I need to press a bit higher than the
            // actual button to hit it." Every setup scenario before this one
            // posed the EXPANDED sheet, so a drawer-height layout fault had
            // nowhere to show up.
            newGame()
            collapseForReview()

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

        case "pass":
            // ROUND 29: CHANNEL A OF THE TRANSFER - MY OWN pass, staged.
            //
            // The other two channels a pass can reach are the same replay of a
            // kernel stream and `HARNESS_SCENARIO=arrival HARNESS_ARRIVE_KIND=pass`
            // poses both (cold with HARNESS_ARRIVE_COLD=1, live without). A
            // STAGED pass is the odd one out and needs its own board: a
            // transfer does not clear the table, so it never becomes a sequence
            // at all - the card flies through `flyPlacement` and the roles are
            // handed over by the `!sequenced` branch of the view's `onChange`,
            // in the same tick. Nothing in the rig could reach that path,
            // because a pass is only ever offered alongside a cover and a
            // pickup and the auto-player takes the first move on the menu.
            //
            // So: drive until the DEFENDER may transfer, then sit me in that
            // seat. Pair with SIMCTL_CHILD_HARNESS_AUTOMOVE=1 and
            // SIMCTL_CHILD_HARNESS_AUTOMOVE_KIND=pass to have the board play it
            // through the same drag/tap path a finger does, and add
            // HARNESS_AUTOSEND=1 for Channel B on top.
            await dealDriven(players: playersEnv, only: nil, steps: 400,
                             viewAs: .defender, stopWhenCanPass: true)

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
    /// - `stopWhenCanPass`: halt as soon as the defender may TRANSFER, and keep
    ///   the transfer off the driver's own menu so it cannot spend the move
    ///   being hunted for. The only flag that re-deals (round 29).
    /// - `viewAs`: whose eyes the screenshot is taken through.
    private func dealDriven(players n: Int, only: [MoveType]?, steps: Int,
                            viewAs: ViewAs, stopWhenDeckEmpty: Bool = false,
                            stopWhenCanPass: Bool = false) async {
        setCount(n)
        let joins = (0..<n).map { MessageJoin(seat: $0, name: Self.nameFor($0)) }
        do {
            var lastSeat = 0
            // ROUND 29: RE-DEAL until one deal produces the board being posed.
            // Only `stopWhenCanPass` asks for it, and it has to: a transfer
            // needs the defender to be holding the rank that is already on the
            // table, which a given deal may never offer at all. Every other
            // scenario keeps the fixed seed 42 - being byte-repeatable is the
            // point of this rig - so the loop runs exactly once for them.
            deal: for salt in 0..<(stopWhenCanPass ? 40 : 1) {
                let seed = salt == 0 ? Data(repeating: 42, count: 32)
                    : Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ salt) | 1 })
                try await MessageKernel.shared.newGame(seed: seed, players: n)
                lastSeat = 0
                for _ in 0..<steps {
                    if stopWhenDeckEmpty,
                       let v = await MessageKernel.shared.residentView(viewer: -1),
                       v.deckCount == 0 || v.isOver { break }
                    // …and stop the moment the DEFENDER may transfer, so the
                    // board this lands on is one where the move being watched
                    // is on my own menu (`viewAs: .defender` then seats me
                    // there).
                    if stopWhenCanPass,
                       let v = await MessageKernel.shared.residentView(viewer: -1), !v.isOver,
                       await MessageKernel.shared.residentLegal(seat: v.defender)
                           .contains(where: { $0.type == .pass }) { break deal }
                    var acted = false
                    for s in 0..<n {
                        let legal = await MessageKernel.shared.residentLegal(seat: s)
                        let pick = legal.first { m in
                            guard m.type != .wait else { return false }
                            // Never spend the move being hunted for: this driver
                            // drives the defender too, so without this it plays
                            // every transfer it finds and the stop above is asked
                            // the instant after the chance has gone.
                            if stopWhenCanPass, m.type == .pass { return false }
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
    ///  - `pass` (round 29): the arriving move is a TRANSFER - the defender
    ///    lays the same rank down and the defence moves on. Four things must
    ///    happen in ONE beat (docs/ANIMATION_CATALOGUE.md): the card flies to
    ///    the table, the shield flies from the passer to the next defender, the
    ///    passer's sword rotates in, and the next defender's rotates out. The
    ///    default watcher is a bystander, who sees the shield cross the whole
    ///    table; pair it with HARNESS_ARRIVE_SELF=1 to watch as the PASSER, whose
    ///    own shield leaves and whose sword turns in behind it.
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
    /// HARNESS_AUTOSEND: once a move has been auto-staged, press Send.
    ///
    /// Pairs with HARNESS_AUTOMOVE, which plays a legal move through the real
    /// tap path. Together they are the half of the app the arrival rig never
    /// touched: STAGE and SEND, which is where both 1.0(23) reports came from
    /// ("i just did a cover. whwn i sent it strangely animated to the state
    /// before the cover"). Polls rather than sleeps a fixed time, because the
    /// stage lands whenever the move's own animation settles.
    private func autoSendWhenStaged() async {
        guard ProcessInfo.processInfo.environment["HARNESS_AUTOSEND"] != nil else { return }
        // Wait for a NEW stage, not just for "something staged". The warm-up
        // leaves a bubble in the input field from an earlier seat, so a poll for
        // non-nil returned instantly and Send delivered THAT - a chain from
        // another player, which `markSent` then rightly refused as bytes this
        // board never sealed ("markSent REFUSED - not my bytes. mine=[t5 r1
        // actor0] sent=[t3 r0 actor1]"). The guard was working; the rig was
        // pressing Send on the wrong bubble.
        //
        // Generous, because the auto-player ahead of this one legitimately
        // waits: for the arriving board to publish a menu at all, and then for
        // the round-16 pickup hold to lapse - up to fifteen seconds of doing
        // exactly what a player does.
        let before = stagedPayloadBytes
        let deadline = Date().addingTimeInterval(45)
        while Date() < deadline, stagedPayloadBytes == before {
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        guard stagedPayloadBytes != nil, stagedPayloadBytes != before else {
            AnimLog.say("scenario: nothing staged to send - rig bug")
            return
        }
        // A beat, so the staged board is on screen before the send moves it -
        // the owner watches this transition, so the rig must too.
        try? await Task.sleep(nanoseconds: 600_000_000)
        AnimLog.say("scenario: pressing Send on the staged move")
        deliver()
        // Let the send land before the oracle reads the board: `markSent`
        // decodes the sent bytes, rebases, and releases any withheld
        // settlement, and that is the very transition being measured.
        try? await Task.sleep(nanoseconds: 2_500_000_000)
    }

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
        case "pass": .pass
        default: .attack
        }
        // `gameover` is `coverend` without the survive-the-bout condition: the
        // defender's last cards go down on an EMPTY deck at two seats, so the
        // same move that closes the bout closes the game. Owner, round 20:
        // "replaying a final move doesn't even show the winning move animation.
        // It should show the final move animation, then the ranks."
        let deep = kind == "coverend" || kind == "gameover"
        // ROUND 29: `pass` - the TRANSFER, and the shape the catalogue had never
        // posed in any channel. The owner's four-part beat (the card, the shield
        // flying to the next defender, the passer's sword rotating in, the next
        // defender's rotating out) is the one animation on the board that
        // nothing had ever watched end to end, in the rig or on a phone.
        //
        // Unlike every other kind it can be SPENT by the warm-up: a transfer is
        // only ever on the defender's menu, and the warm-up drives whatever it
        // finds there, so the seat this rig is waiting for would play the very
        // move being waited for. Hence `transfer` below, which keeps the pass
        // off the warm-up's menu, and the re-deal loop `deep` already has - not
        // every hand contains a transfer at all (the defender has to be holding
        // the rank that is on the table), so a single deal is a coin toss.
        let transfer = kind == "pass"
        // ROUND 36: HARNESS_ARRIVE_BIGHAND=<n> warms up until somebody is
        // holding at least `n` cards, which is the only way to pose the owner's
        // row-split reports at all.
        //
        // "I had 10 of diamonds covering 8 of diamonds. The cards were in the
        // skinny card one row layout. The other player then hit good. Around
        // the time the check started rotate animating, the cards started to
        // animate towards the two row layout, then like changed their mind mid
        // layout transition and went back to the skinny card one row layout.
        // Why the fuck did that happen? We had ten cards and they said good!"
        //
        // FHandFan splits into two rows once a card would be thinner than 34pt,
        // which at a phone's hand width is somewhere around ten cards - so every
        // arrival run before this one watched a hand that could not split, and
        // the whole class was invisible to the rig. The warm-up below therefore
        // PREFERS pickups while it is under the target, because a pickup is the
        // only move in Durak that makes a hand bigger by more than one.
        let bigHand = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_BIGHAND"] ?? "") ?? 0
        func biggestHand() async -> Int {
            guard let v = await MessageKernel.shared.residentView(viewer: -1) else { return 0 }
            return v.players.map(\.handCount).max() ?? 0
        }
        // Is the wanted ARRIVAL playable right now? For `goodend` the good must
        // actually CLOSE the bout, which means a non-empty, fully covered table
        // - a good over open attacks merely passes priority and animates
        // nothing, which is not the arrival being posed.
        func wantReady() async -> Int? {
            guard let v = await MessageKernel.shared.residentView(viewer: -1) else { return nil }
            // Not until somebody's hand can actually SPLIT - see `bigHand`. The
            // arrival is only interesting on a board where the row count is in
            // play, so a run that posed it over a six-card hand would pass while
            // testing nothing.
            if bigHand > 0, (v.players.map(\.handCount).max() ?? 0) < bigHand { return nil }
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
        // …and a bighand run needs room to take several times over, so it gets
        // the deep cap even for a shallow kind.
        let cap = deep || bigHand > 0 ? 400 : 40
        // …and if one whole game goes by without producing the board, RE-DEAL.
        // Not every deal contains a bout-ending cover at all (the defender has
        // to run out on a table they can fully answer), so a single game is a
        // coin toss - the offline finder loops 40 seeds for the same reason. A
        // TRANSFER is the same coin toss for the same kind of reason (the
        // defender has to be holding the rank that is already on the table), so
        // it re-deals too. Any other kind takes exactly one pass, as it always
        // has.
        let deals = deep || transfer ? 40 : 1
        // Grow a hand: take the table rather than defend it, and open bouts
        // rather than end them, until somebody is over the split threshold.
        // Returns nil once the target is reached, which hands the warm-up back
        // to its ordinary "first legal move" driver.
        func bigHandPick(_ legal: [Move], _ biggest: Int) -> Move? {
            guard bigHand > 0, biggest < bigHand else { return nil }
            return legal.first { $0.type == .pickup } ?? legal.first { $0.type == .attack }
        }
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
                        // ROUND 29: never spend the move being hunted for. A
                        // transfer only ever appears on the DEFENDER's menu, and
                        // this loop drives the defender too - so without the
                        // second clause the warm-up plays every pass it finds
                        // and `wantReady` is asked the instant after the only
                        // seat that could transfer has stopped being able to.
                        : bigHandPick(legal, await biggestHand())
                            ?? legal.first { $0.type != .wait && !(transfer && $0.type == .pass) }
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
        // ROUND 22: HARNESS_ARRIVE_SELF=1 watches as the seat that MOVED - "I
        // close the bubble I just sent and open it again", which is what the
        // owner did when they reported the missing sword-to-good rotation. Every
        // other run here deliberately watches somebody who did NOT move, so
        // reopening one's own move was a case this rig could not pose at all.
        // Only meaningful with HARNESS_ARRIVE_COLD=1: live, a mover's own board
        // already holds its role marks in @State and there is nothing to re-seed.
        let watchSelf = ProcessInfo.processInfo.environment["HARNESS_ARRIVE_SELF"] == "1"
        let watcher = watchSelf
            ? mover
            : ((0..<n).first { $0 != mover && $0 != view.defender } ?? ((mover + 1) % n))

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
        // ROUND 29: …AND UNTIL IT HAS ACTUALLY STOPPED MOVING.
        //
        // The sleep above is a guess at how long a mount takes, and it is not
        // always enough: opening the parent chain REPLAYS its last turn, and a
        // turn that folded several actions into one bubble (a pickup, its
        // refills, the roles) runs for many seconds. The arrival then landed in
        // the middle of that replay, which supersedes it - so the run posed "a
        // bubble arrives mid-animation" (a real case, and one HARNESS_ARRIVE_N
        // poses deliberately) instead of the one asked for, on a board whose
        // marks and counts were still those of the bubble BEFORE the one on
        // screen. Round 29 hit this trying to watch a TRANSFER arrive and got a
        // shield handed over from a defender two moves stale.
        //
        // Bounded by `waitForSettle`'s own 8s timeout, so a wedged sequence
        // still lets the run finish and report. Only the FIRST arrival waits:
        // the burst cases below are meant to interrupt each other.
        if !cold { await BoardAnimator.waitForSettle() }

        // ROUND 30: pose the arrival against the COMPACT DRAWER, not the
        // expanded board. Every arrival run so far watched a full-height board,
        // and the owner's 1.0(29) geometry report is from the other one: "in
        // the collapsed view ... the animation seemed to go to the table center
        // rather than their hand". The drawer is where most play actually
        // happens, and it is a different set of frames - a shorter board moves
        // every landmark a flight aims at.
        //
        // AFTER the settle, so the collapse cannot be what the first replay
        // races against; the arrival below then lands on a board that has been
        // compact and still for a beat, which is the owner's case exactly.
        if ProcessInfo.processInfo.environment["HARNESS_ARRIVE_COMPACT"] == "1" {
            AnimLog.say("scenario: collapsing to the compact drawer before the arrival")
            collapseForReview()
            // HARNESS_ARRIVE_COMPACT_MS: how long after the collapse the
            // arrival lands. The default waits the tween out; a small value
            // poses the case the owner actually hit - a sequence starting
            // WHILE the box is still easing down from the expanded height,
            // which is when a flight aimed once is aimed at a board that no
            // longer exists.
            let ms = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_COMPACT_MS"] ?? "") ?? 1500
            try? await Task.sleep(nanoseconds: UInt64(max(ms, 0)) * 1_000_000)
        }

        // …now the other seats play, and each ARRIVES. `HARNESS_ARRIVE_N` (with
        // `HARNESS_ARRIVE_GAP` in milliseconds) is the case the owner reports:
        // bubbles landing one after another on a board that is still animating
        // the last one, which is what a real table does when two people are
        // playing at once. A gap SHORTER than a flight is the point.
        //
        // HARNESS_ARRIVE_STAGED=1 - THE COMMONEST CONFLICT ON A REAL TABLE
        // (docs/ANIMATION_CATALOGUE.md, "An arrival while I have a move
        // staged"): an arrival lands on a board with an UNSENT move on it. It
        // cannot be posed before the first arrival - at two seats the watcher
        // has nothing legal until somebody moves - so the shape is the natural
        // one: arrival 1 lands, HARNESS_AUTOMOVE stages the watcher's reply
        // through the real tap path, and only once that stage is real is the
        // NEXT arrival composed and delivered onto it. The conflict model must
        // retract the staged card in RED against the OLD base before the
        // arriving chain plays; the CONFLICT oracle at the bottom says whether
        // that machinery actually engaged. Forces at least two arrivals, since
        // the conflict is between them.
        let stagedConflict = ProcessInfo.processInfo.environment["HARNESS_ARRIVE_STAGED"] == "1"
        let n_asked = Int(ProcessInfo.processInfo.environment["HARNESS_ARRIVE_N"] ?? "1") ?? 1
        let n_arrivals = stagedConflict ? max(n_asked, 2) : n_asked
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
            // ARRIVE_STAGED: from the second arrival on, hold delivery until
            // the watcher's auto-played reply is genuinely STAGED, so this
            // arrival provably lands on a board with an unsent move. Waiting
            // AFTER the adopt handshake above, because the stage is automove's
            // REACTION to the previous arrival.
            if stagedConflict, i > 0 {
                let stagedBy = Date().addingTimeInterval(20)
                while Date() < stagedBy, stagedPayloadBytes == nil {
                    try? await Task.sleep(nanoseconds: 100_000_000)
                }
                AnimLog.say(stagedPayloadBytes == nil
                    ? "scenario: nothing staged before arrival \(i + 1) - rig bug (set HARNESS_AUTOMOVE=1)"
                    : "scenario: watcher has a move STAGED - arrival \(i + 1) must retract it in red")
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
                // Ordinarily the watcher must NOT be the one moving - the whole
                // point is somebody else's move landing on my board. With
                // HARNESS_ARRIVE_SELF the rule inverts: the bubble to open is my
                // OWN, so only the watcher may act.
                if watchSelf { guard s == watcher else { continue } }
                else { guard s != watcher else { continue } }
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
        // ROUND 22: and now MY OWN half of a turn - stage, then send. Pair
        // HARNESS_AUTOMOVE (which plays a legal move through the real tap path)
        // with HARNESS_AUTOSEND and the oracle below measures the board AFTER a
        // send instead of after an arrival. That is the half of the app both
        // 1.0(23) reports came from, and until now no rig run touched it: the
        // harness's Send did not even signal the board (HarnessModel.deliver).
        // A no-op unless HARNESS_AUTOSEND is set, so every existing run is
        // unchanged.
        await autoSendWhenStaged()

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
            + "VANISHED paints: \(MessageTableView.vanishedAtRest) "
            + "STRANDED paints: \(MessageTableView.strandedAtRest) "
            + "SWEEP-STILL-DRAWN: \(MessageTableView.sweepVisibleNow) "
            // ROUND 40: the veil still standing this long after the last move.
            // Must be 0 - anything else is a card laid out nowhere, which the
            // CENTRED fan renders as a row at the wrong width and centre. See
            // `MessageTableView.veilStandingNow`.
            + "VEIL-STILL-UP: \(MessageTableView.veilStandingNow)")
        // THE CONFLICT ORACLE (1.0(28)): did the conflict machinery engage?
        // `retractions` counts staged moves visibly retracted (an ARRIVE_STAGED
        // run must show >= 1 or the model never fired); `red-flights` counts
        // red ghosts flown, retractions and sequence reversals both.
        // `retracting` must be false at rest - a retraction still standing this
        // long after the last arrival is one that never released its latched
        // adopt. `stagedLeft` is context, not an assertion: with AUTOMOVE the
        // watcher legitimately re-stages a reply to the NEWEST board after the
        // conflict, exactly as a real player would.
        AnimLog.say("CONFLICT retractions: \(MessageTableView.conflictRetractions) "
            + "red-flights: \(MessageTableView.redRevertFlights) "
            + "stagedLeft: \(MessageTurnController.debugLatest?.pending.count ?? -1) "
            + "retracting: \(MessageTurnController.debugLatest?.conflictRetracting ?? false)")
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
