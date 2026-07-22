// MessagesRootView — what the extension shows, per presentation style (§10).
//
// Compact is the KEYBOARD AREA (§3.5): no text field, no horizontal scrollers —
// so it is a label and buttons. Expanded is the table. The routing here is the
// §5/§6/§7 machine wearing a UI: a selected bubble is decoded + adopted, my seat
// is resolved (§6), and I either play (MessageTableView, staging a reply) or,
// when three-plus players leave my seat ambiguous, pick who I am (§6.3). New game
// opens a lobby where I am seat 0 (§5.2/lobby v3, docs/IMESSAGE_LOBBY_V3.md) —
// every chat shape, DM included, locks its seed at create and deals nobody in
// until Start.
//
// No Durak rule is answered in this file — MessageTurnController relays the kernel
// and MessageComposer only stages. Seat identity is the one non-kernel decision,
// and it is SeatIdentity's pure §6 logic, fed the conversation's `senderIsLocal`.
import SwiftUI

/// The extension's two presentation states, decoupled from the Messages
/// framework so this view compiles into FoolishKit and is drivable by both the
/// real `MessagesViewController` (which maps `MSMessagesAppPresentationStyle`
/// onto it) AND the FoolishHarness test app (§ harness). Nothing here imports
/// `Messages`.
public enum MsgPresentation { case compact, expanded }

public struct MessagesRootView: View {
    let payloadURL: URL?
    let style: MsgPresentation
    let senderIsLocal: Bool
    let startNewGame: Bool
    /// Bumped by the host each time the human taps New game, so an explicit New
    /// game resets the session while a mere compact<->expanded toggle does not.
    let newGameToken: Int
    /// This conversation's identity (`ChatKey.make` over its participant set),
    /// threaded down to every `MessageGameStore` lookup so a game cached from a
    /// DIFFERENT chat on this device can never resolve `.known` here — see the
    /// chat-scoping fix in `MessageGameStore`'s type doc.
    let chatKey: String
    let chatIsDM: Bool
    let chatPlayers: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    let onSend: (Data, Int) async -> Void
    /// Retract a previously-staged bubble (§10 undo). No-op by default so every
    /// existing caller keeps compiling; the real extension has no API to remove an
    /// inserted input-field bubble, so it can only drop its own pending-stage record.
    let onUnstage: () -> Void

    public init(payloadURL: URL?, style: MsgPresentation, senderIsLocal: Bool,
                startNewGame: Bool, newGameToken: Int = 0, chatKey: String, chatIsDM: Bool,
                chatPlayers: Int,
                requestExpand: @escaping () -> Void, onNewGame: @escaping () -> Void,
                onSend: @escaping (Data, Int) async -> Void,
                onUnstage: @escaping () -> Void = {}) {
        self.payloadURL = payloadURL; self.style = style; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.newGameToken = newGameToken
        self.chatKey = chatKey; self.chatIsDM = chatIsDM; self.chatPlayers = chatPlayers
        self.requestExpand = requestExpand; self.onNewGame = onNewGame; self.onSend = onSend
        self.onUnstage = onUnstage
    }

    public var body: some View {
        // ONE surface for both presentation styles — NOT a compact/expanded switch.
        // The switch made SwiftUI destroy the expanded @State (the whole in-progress
        // game) whenever you dragged to the compact drawer, so the two sizes looked
        // like two separate games (B4 bug). GameSurface is always the root's child,
        // so its game state survives a style change; it renders the SAME table in
        // both, just sized to the strip (compact) or full-screen (expanded).
        GameSurface(payloadURL: payloadURL, style: style, senderIsLocal: senderIsLocal,
                    startNewGame: startNewGame, newGameToken: newGameToken, chatKey: chatKey,
                    chatIsDM: chatIsDM, chatPlayers: chatPlayers,
                    requestExpand: requestExpand, onNewGame: onNewGame, onSend: onSend,
                    onUnstage: onUnstage)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(WoolBackground())          // the table surface, not system white
    }
}

private struct GameSurface: View {
    let payloadURL: URL?
    let style: MsgPresentation
    let senderIsLocal: Bool
    let startNewGame: Bool
    let newGameToken: Int
    let chatKey: String
    let chatIsDM: Bool
    let chatPlayers: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    let onSend: (Data, Int) async -> Void
    let onUnstage: () -> Void

    /// A phase-0/handoff lobby the extension shows instead of the board (§5.2).
    private struct Lobby { let env: MessageEnvelope; let payload: Data }
    /// A resolved seat waiting on the human's name (§B3). The 2-player receiver
    /// reaches a board with no name set — the creator named themselves in setup and
    /// 3-8p joiners in the lobby, but the DM opponent has neither. Ask once, store
    /// it, then seat them; every later game reuses the stored name.
    private struct NameGate { let env: MessageEnvelope; let payload: Data; let seat: Int
                              let survivors: [Move]; let discarded: Int
                              let prevPayload: Data? }   // note 4/9/38: threaded to seatOnBoard

    @State private var controller: MessageTurnController?
    @State private var ambiguous: (env: MessageEnvelope, payload: Data)?
    /// RELEASE-ONLY substitute for `ambiguous` (§6.3): an unresolved identity in
    /// Release must never offer a seat picker (anyone could claim any hand), so we
    /// show the same PUBLIC spectator board a delivered bubble's snapshot uses,
    /// instead. DEBUG keeps the real picker (single-simulator testing needs it).
    @State private var spectator: (view: GameView, names: [Int: String])?
    @State private var lobby: Lobby?
    @State private var nameGate: NameGate?
    @State private var showSetup = false
    @State private var toast: String?
    @State private var damaged = false

    /// A style toggle keeps this key stable, so the session is NOT reloaded and
    /// the in-progress game survives. A new bubble (payloadURL) or a New game tap
    /// (newGameToken) changes it, which resets and reloads. `chatKey` is in here
    /// too, defensively: this view's state must never survive a conversation
    /// change (the chat-scoping fix's whole point), even though in practice one
    /// extension instance presents one conversation for its lifetime.
    private var loadKey: String {
        "\(newGameToken)|\(startNewGame)|\(chatKey)|\(payloadURL?.absoluteString ?? "")"
    }

    var body: some View {
        // One game per chat, one surface for both presentation styles: always the
        // table (or the New-game setup when the thread has no game yet). There is no
        // "A game in this thread / Open the game" menu — collapsing to compact just
        // shows the same table in the short strip, with Messages' Send in the
        // compose area above. Keeping a single `expandedContent` root also means the
        // board's @State + .task survive the expanded<->compact toggle.
        expandedContent
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .fToast($toast)
            .task(id: loadKey) {
                await reloadForInput()
                await autoDriveLobby()
            }
    }

    @ViewBuilder private var expandedContent: some View {
        if let controller {
            MessageTableView(controller: controller,
                             onSend: { payload in await onSend(payload, controller.mySeat) },
                             onNewGame: onNewGame,
                             onUnstage: onUnstage)
        } else if let lob = lobby {
            LobbyView(env: lob.env, mySeat: lobbySeat(lob.env),
                      nickname: MessageGameStore.shared.nickname,
                      onJoin: { name in Task { await joinLobby(lob, nickname: name) } },
                      onStart: { Task { await startGame(lob) } },
                      onInvite: { Task { await onSend(lob.payload, lobbySeat(lob.env) ?? 0) } })
        } else if let g = nameGate {
            NameGateView(prefill: MessageGameStore.shared.nickname) { name in
                Task { await nameThenSeat(name, gate: g) }
            }
        } else if showSetup {
            // chatPlayers is threaded through unused (see NewGameSetup's own doc)
            // — kept only so this call site, the harness, and
            // MessagesViewController (which all compute a real participant
            // count) keep compiling unchanged.
            NewGameSetup(nickname: MessageGameStore.shared.nickname,
                         isDM: chatIsDM, chatPlayers: chatPlayers) { name in
                Task { await start(nickname: name) }
            }
        } else if let a = ambiguous {
            SeatPicker(nPlayers: a.env.nPlayers, joins: a.env.joins) { seat in
                Task { await choose(seat: seat, from: a) }
            }
        } else if let s = spectator {
            // Release-only §6.3 fallback: read-only, public (no hand), with a
            // caption explaining why there is nothing to tap (§ release security).
            VStack(spacing: 4) {
                MessageBoardView(view: s.view, names: s.names)
                Text(FStrings.t("ios.msg.spectating"))
                    .font(.footnote).foregroundStyle(.black.opacity(0.55))
                    .multilineTextAlignment(.center).padding(.horizontal).padding(.bottom, 8)
            }
        } else if damaged {
            DamagedView()
        } else {
            ProgressView()
        }
    }

    /// Reset + (re)load for a NEW input. A compact<->expanded toggle leaves
    /// loadKey unchanged, so `.task(id:)` does not fire and the game persists.
    private func reloadForInput() async {
        AnimLog.say("surface reload key=[\(loadKey)]")
        controller = nil; lobby = nil; nameGate = nil; showSetup = false
        ambiguous = nil; spectator = nil; damaged = false
        await load()
        AnimLog.say("surface showing \(showingWhat)")
    }

    /// What the surface resolved to, for the trace. "Why is it showing a lobby
    /// when the thread is mid-game" is only answerable if the surface says which
    /// branch it took and off which bytes.
    private var showingWhat: String {
        if controller != nil { return "board" }
        if let l = lobby { return "lobby(joins=\(l.env.joins.count) phase=\(l.env.phase) game=\(l.env.gameId))" }
        if nameGate != nil { return "nameGate" }
        if showSetup { return "setup" }
        if ambiguous != nil { return "seatPicker" }
        if spectator != nil { return "spectator" }
        if damaged { return "damaged" }
        return "nothing"
    }

    /// Ask the router what to show, then put it on screen. The DECISION —
    /// setup vs lobby vs board, and which chain wins Rule P — is not made here
    /// any more (MessageSurfaceRouter): it is a function of the selected
    /// bubble, this chat's cache, and the New-game intent, so it can be driven
    /// in a test without a simulator. What stays here is the part that genuinely
    /// needs the host: seat identity (§6), the name gate, and Rule R's rebase.
    private func load() async {
        AnimLog.say("surface load url=\(payloadURL?.absoluteString.suffix(12) ?? "nil") startNew=\(startNewGame)")
        var incoming: Data?
        if let url = payloadURL {
            guard let bytes = try? MessageEnvelope.payloadBytes(url: url) else {
                damaged = true
                return
            }
            incoming = bytes
        }
        let screen = await MessageSurfaceRouter.resolve(payload: incoming,
                                                        startNewGame: startNewGame,
                                                        chatKey: chatKey)
        AnimLog.say("surface router -> \(screen)")
        switch screen {
        case .setup:
            showSetup = true
        case .damaged:
            damaged = true
        case .lobby(let payload):
            // Decoding also ADOPTS, so the lobby's locked seed is resident for a
            // join/start seal — same as before this was routed.
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1) else {
                damaged = true
                return
            }
            lobby = Lobby(env: env, payload: payload)
        case .board(let payload):
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1) else {
                damaged = true
                return
            }
            await adopt(winner: payload, env: env)
        }
    }

    /// DEV ONLY (HARNESS_AUTOGAME): press the setup/lobby buttons a human would,
    /// so an unattended run can actually reach a board. Lobby v3 put three human
    /// taps — Create game, Join, Start — between launch and a dealt game, and the
    /// harness's auto-play only knows how to make MOVES, so an auto-run just sat
    /// on the setup screen forever and the animation trace it exists to produce
    /// was four lines long. Each participant's turn through here does the one
    /// thing that seat can do; HARNESS_AUTOGAME's own deliver+become carries it
    /// to the next. Never compiled into Release.
    ///
    /// Runs INSIDE `.task(id: loadKey)`, not as a Task of its own, and that is
    /// load-bearing: a detached one outlives the surface that started it. The
    /// first version was detached, and its 400ms sleep regularly finished after
    /// the harness had already switched to the next participant — so a joiner's
    /// pending drive ran with the PREVIOUS player's captured lobby and started
    /// the game as them. An 8-player run reached a 2-player board with a seat
    /// nobody at that keyboard held. Under `.task` it is cancelled with the
    /// surface, so a stale drive cannot act at all.
    ///
    /// It also waits for the lobby to FILL. Starting at two is what a human may
    /// do, but an auto-run that does it turns "8 players" into a 2-player game
    /// and never exercises the seat count being asked about.
    private func autoDriveLobby() async {
        #if DEBUG
        guard ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] != nil else { return }
        try? await Task.sleep(nanoseconds: 400_000_000)
        if Task.isCancelled { return }
        if showSetup { await start(nickname: MessageGameStore.shared.nickname); return }
        guard let lob = lobby else { return }
        // The lobby's capacity is the WIRE's max (8) for a group, not how many
        // people are in the chat — so the target is the chat's own size.
        let target = min(lob.env.nPlayers, max(2, chatPlayers))
        if lobbySeat(lob.env) == nil {
            if lob.env.joins.count < lob.env.nPlayers {
                await joinLobby(lob, nickname: MessageGameStore.shared.nickname)
            }
        } else if lob.env.joins.count >= target {
            await startGame(lob)
        }
        #endif
    }

    // MARK: creation + lobby (§5.2)

    /// Finish the New game setup: persist the nickname (B3), then create a
    /// lobby — every chat shape now goes through the SAME lobby machinery
    /// (lobby v3, note 2: "2p — creator creates the game and sends the first
    /// chat. The other player can join, or do join+start... the same hand
    /// because the seed was set by the first chat the creator sent"). A DM
    /// used to deal LIVE straight to the board here (`startGenesis`, now
    /// removed) — that let the creator see their hand before committing and
    /// reroll by tapping New game until it was good; a locked-seed lobby
    /// closes that.
    private func start(nickname: String) async {
        MessageGameStore.shared.nickname = nickname
        showSetup = false
        await createWaiting(nickname: nickname)
    }

    /// Create a game as seat 0 and open its lobby (lobby v3): lock the seed +
    /// game id in NOW — that is the whole "seed locked at create" guarantee —
    /// and seal a WAITING bubble seating only me. The kernel is dealt at the
    /// lobby's CAPACITY, not a chosen player count: nobody has picked how many
    /// will play yet. For a group chat that capacity is the wire's max, 8 (a
    /// WAITING envelope with n_players==8 renders as an open lobby, not 8
    /// literal seats — see LobbyView) — not a real 8-player game. A DM's
    /// capacity is 2 (note 2): the chat has exactly two people, so "lobby
    /// full" must read correctly once the one possible opponent has joined,
    /// not "waiting for 6 more". Start (below) later re-derives the SAME seed
    /// at however many actually joined. Auto-stages the invite (notes 14/16):
    /// the human still presses Messages' own Send, but there is no separate
    /// "Send invite" button offering the same action a second time.
    private func createWaiting(nickname: String) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        let capacity = chatIsDM ? 2 : 8
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            let joins = [MessageJoin(seat: 0, name: nickname)]
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: 0, env: env, payload: payload)
            lobby = Lobby(env: env, payload: payload)
            await onSend(payload, 0)
        } catch {
            damaged = true
        }
    }

    /// My seat in a lobby, or nil if I have not claimed one yet (§6). Note 14:
    /// gated through `SeatIdentity.resolveInLobby`, not the plain `resolve` the
    /// live board uses — see that function's doc for the bug this closes (a
    /// stale lobby bubble granting Start/Send to a seat it doesn't list, and
    /// the flip side, a fresh join not showing as joined). Note 15's Rule-P-
    /// for-lobbies fix in `load()` means the NEWEST bubble (the one that really
    /// does list me) is what gets shown here in the first place.
    private func lobbySeat(_ env: MessageEnvelope) -> Int? {
        SeatIdentity.resolveInLobby(
            cachedSeat: MessageGameStore.shared.seat(gameId: env.gameId, chatKey: chatKey),
            senderIsLocal: senderIsLocal, nPlayers: env.nPlayers,
            lastActorSeat: env.lastActorSeat, joins: env.joins)
    }

    /// Claim the lowest free seat (§5.2, lobby v3). Always reseals WAITING and
    /// stays in the lobby — joining NEVER starts the game, no matter how many
    /// have joined or that the lobby's own capacity (8 for a group, 2 for a DM
    /// — see `createWaiting`) is reached; Start (below) is the one, explicit
    /// action that flips the game LIVE. Auto-stages the reseal (notes 14/16):
    /// the human still presses Messages' own Send, there is no separate "Send
    /// invite" button.
    private func joinLobby(_ lob: Lobby, nickname: String) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        let trimmed = nickname.trimmingCharacters(in: .whitespaces)
        let nick = trimmed.isEmpty ? FStrings.t("ios.you") : trimmed
        MessageGameStore.shared.nickname = nick   // remember it for the next game (B3)
        let joins = (env.joins + [MessageJoin(seat: free, name: nick)]).sorted { $0.seat < $1.seat }
        do {
            // Re-adopt the lobby so the LOCKED seed + open capacity are resident
            // for the seal.
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: free, env: newEnv, payload: payload)
            await onSend(payload, free)
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }

    /// Start the game at the ACTUAL joined count (§5.2, lobby v3). Any JOINED
    /// player may do this once 2+ have joined (LobbyView gates the button on
    /// that; nothing re-checks it here — the kernel would happily reseat and
    /// seal a 1-player "game" too, but the design never offers the button for
    /// it). Re-derives the resident game from the seed LOCKED at create, at
    /// `joins.count` seats — contiguous 0..<k because seats are always claimed
    /// lowest-free-first — then seals the LIVE handoff (turn 0, parent8 =
    /// first8(lobby digest), the same joins) and drops the starter onto the
    /// board: mechanically identical to what the OLD "last joiner auto-starts"
    /// branch of `joinLobby` used to do, just triggered explicitly instead of
    /// implicitly by seat count. Uses the shared `MessageKernel.startFromLobby`
    /// primitive so this reseat/seal is provably the deal locked at create.
    private func startGame(_ lob: Lobby) async {
        let env = lob.env
        guard let seat = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        do {
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.startFromLobby(
                lobbyPayload: lob.payload, gameId: gid, actingSeat: seat,
                parent8: parent, joins: env.joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: seat, env: newEnv, payload: payload)
            await onSend(payload, seat)
            controller = MessageTurnController(parentPayload: payload, parent: newEnv, mySeat: seat)
            lobby = nil
        } catch {
            damaged = true
        }
    }

    /// Adopt `winner` as the game, rebase my staged-but-unsent moves onto it
    /// (Rule R, §7.4), refresh the preferred-chain cache, and open the board.
    private func adopt(winner: Data, env: MessageEnvelope) async {
        // A WAITING envelope is an INVITE, and this function opens a BOARD. They
        // are never interchangeable: a lobby seal leaves a game dealt at the
        // lobby's CAPACITY resident (8 for a group chat — see `createWaiting`),
        // so adopting one as a board shows a phantom 8-player game whose unjoined
        // seats read "Seat N", with a different first attacker than the real
        // game — the round-3 "some see a 5-player game, some see 8" fork, which
        // deadlocks the thread. Rule P now ranks any started chain above a lobby
        // (msg_rule_p rule 0), so nothing should reach here at phase 0 any more;
        // this is the structural guarantee behind that, not a second opinion
        // about which chain wins.
        if env.phase == 0 { lobby = Lobby(env: env, payload: winner); return }
        // note 4/9/38: MessageGameStore still holds the chain we PREVIOUSLY
        // cached for this game — `cache(...)` (via seatOnBoard/choose below)
        // is what overwrites it. Grab its raw bytes now, before that happens,
        // so the controller can later diff its own resolved seat's hand +
        // replay-log count against it (that decode happens seat-aware, inside
        // the controller, once `mySeat` is actually known — see
        // MessageTurnController.begin()). Only "nothing cached yet" leaves
        // this nil (a genuine first-ever open, which falls back to
        // MessageTableView's trailing-run heuristic).
        //
        // Note 13: `bytes == winner` — a REOPEN of a chain we already fully
        // cached — used to be excluded here too ("not the same chain" was
        // the old condition), on the reasoning that there's nothing to diff.
        // That's wrong: it IS a real diff, of exactly zero. Passing it
        // through lets MessageTurnController.begin() see equal log counts on
        // both sides and resolve to an EMPTY replay window (see its
        // `openReplayFromLog` doc) instead of silently reporting "no info" —
        // which used to fall through to the SAME structural heuristic a
        // genuine cache miss uses, one with no memory of what it already
        // showed, so a pickup/draw sequence replayed again sometimes and not
        // others depending on whether the table happened to read empty.
        var prevPayload: Data?
        if let prevRow = MessageGameStore.shared.record(gameId: env.gameId, chatKey: chatKey),
           let bytes = Base32.decode(prevRow.payloadBase32) {
            prevPayload = bytes
        }
        // Make the resident game the winner and set Rule R's round guard, then
        // rebase the pending ledger onto it.
        _ = try? await MessageKernel.shared.decode(payload: winner, viewer: -1)
        #if DEBUG
        // Single-simulator harness: both conversations share ONE App Group cache
        // and participant identity, so a received bubble always resolves to the
        // SENDER's seat and you can never view the receiver ("Waiting for Seat 2"
        // while you ARE seat 2). In DEBUG, ask who you are so both seats are
        // playable on one sim. Release resolves automatically (real devices have
        // separate caches + distinct participant UUIDs) and never shows this.
        if MessageDebugFlags.pickSeatOnAdopt { ambiguous = (env, winner); return }
        #endif
        let (survivors, discarded) = await rebasePending(gameId: env.gameId, adoptedRound: env.round)

        switch SeatIdentity.resolve(cachedSeat: MessageGameStore.shared.seat(gameId: env.gameId, chatKey: chatKey),
                                    senderIsLocal: senderIsLocal,
                                    nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat) {
        case .known(let seat):
            // §B3: a player about to be seated who has never chosen a name is asked
            // once (the 2-player receiver has no setup/lobby screen). Creator +
            // lobby joiners already set theirs, so this only fires for the DM
            // opponent's first game; it never re-asks once stored.
            if !MessageGameStore.shared.hasSetNickname {
                nameGate = NameGate(env: env, payload: winner, seat: seat,
                                    survivors: survivors, discarded: discarded, prevPayload: prevPayload)
            } else {
                seatOnBoard(seat: seat, env: env, winner: winner,
                            survivors: survivors, discarded: discarded, prevPayload: prevPayload)
            }
        case .ambiguous:
            #if DEBUG
            // Single-simulator testing keeps the real picker (see the DEBUG note
            // above in this function) — this branch is unreachable in DEBUG anyway
            // because `pickSeatOnAdopt` already returned above, but stays correct
            // if that flag is ever turned off.
            ambiguous = (env, winner)
            #else
            // RELEASE SECURITY: an ambiguous identity must never offer a seat
            // picker — anyone could claim any hand and see it. Show the same
            // PUBLIC spectator board a delivered bubble's snapshot uses instead
            // (§10, MessageBoardView is public-safe by construction). `winner` was
            // already decoded/adopted above, so the resident game IS this chain.
            let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
            if let view = await MessageKernel.shared.residentView(viewer: -1) {
                spectator = (view, names)
            } else {
                damaged = true
            }
            #endif
        }
    }

    /// Open the board for a resolved seat: cache it, surface any Rule R rebase
    /// toast, and hand the winner chain to a fresh controller with the survivors
    /// pre-staged. The tail of `adopt`'s `.known` branch, shared with the name gate.
    private func seatOnBoard(seat: Int, env: MessageEnvelope, winner: Data,
                             survivors: [Move], discarded: Int, prevPayload: Data? = nil) {
        cache(seat: seat, env: env, payload: winner)
        toast = rebaseToast(survivors: survivors, discarded: discarded)
        controller = MessageTurnController(parentPayload: winner, parent: env, mySeat: seat,
                                           preStaged: survivors, prevPayload: prevPayload)
    }

    /// The human answered the name gate: persist the name (blank falls back to the
    /// neutral default, which still counts as "set" so we never re-ask), then seat
    /// them. The name is baked into `joins` when they first play (sealJoins).
    private func nameThenSeat(_ raw: String, gate g: NameGate) async {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        MessageGameStore.shared.nickname = trimmed.isEmpty ? FStrings.t("ios.you") : trimmed
        nameGate = nil
        seatOnBoard(seat: g.seat, env: g.env, winner: g.payload,
                    survivors: g.survivors, discarded: g.discarded, prevPayload: g.prevPayload)
    }

    /// Rule R (§7.4): replay each pending action onto the just-adopted chain. A
    /// re-applied move survives as a staged move to re-send; a discarded one is
    /// gone (round closed, or the new state refuses it). The ledger is rewritten
    /// to exactly the survivors, re-tagged to the adopted round.
    private func rebasePending(gameId: String, adoptedRound: Int) async -> (survivors: [Move], discarded: Int) {
        var survivors: [Move] = []
        var kept: [PendingAction] = []
        var discarded = 0
        for p in MessageGameStore.shared.pending(gameId: gameId) {
            let awire = MoveWire.encodeAction(p.move)
            let verdict = awire.isEmpty ? MessageKernel.Rebase.discardedIllegal
                : ((try? await MessageKernel.shared.rebase(pendingRound: p.round, seat: p.seat, awire: awire))
                   ?? .discardedIllegal)
            if verdict == .reapplied {
                survivors.append(p.move)
                // Re-tagged to the adopted round: it is now composed against THIS
                // chain, so a later round closure guards it correctly.
                kept.append(PendingAction(seat: p.seat, round: adoptedRound, move: p.move))
            } else {
                discarded += 1
            }
        }
        MessageGameStore.shared.setPending(kept, gameId: gameId)
        return (survivors, discarded)
    }

    private func rebaseToast(survivors: [Move], discarded: Int) -> String? {
        if !survivors.isEmpty { return FStrings.t("ios.msg.rebased") }
        if discarded > 0 { return FStrings.t("ios.msg.superseded") }
        return nil
    }


    /// §6.3 pick resolved: remember the seat, rebase, then play. DEBUG-only
    /// single-simulator path (never compiled into Release): deliberately skips
    /// the open-delta-replay hint (`prevPayload` stays nil below) rather than
    /// duplicating `adopt`'s prev-chain lookup for a testing-only picker that
    /// runs before that lookup would even happen (`pickSeatOnAdopt` returns
    /// out of `adopt` first) — this seat opens with the plain fallback replay
    /// instead (notes 4/9/38's cache-miss path), which is a fine trade for a
    /// dev aid.
    private func choose(seat: Int, from a: (env: MessageEnvelope, payload: Data)) async {
        cache(seat: seat, env: a.env, payload: a.payload)
        let (survivors, _) = await rebasePending(gameId: a.env.gameId, adoptedRound: a.env.round)
        controller = MessageTurnController(parentPayload: a.payload, parent: a.env, mySeat: seat,
                                           preStaged: survivors)
        ambiguous = nil
    }

    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        MessageGameStore.shared.put(MessageGameRecord(
            gameId: env.gameId, chatKey: chatKey, mySeat: seat, nPlayers: env.nPlayers, round: env.round,
            turn: env.turn, phase: env.phase, finished: env.phase == 3, names: names,
            payloadBase32: Base32.encode(payload), updatedAt: Date().timeIntervalSince1970))
    }
}

/// New game setup (§5.2, rewritten for lobby v3 — notes 2/19/25). The creator
/// names themselves (B3 — the one place a nickname is entered; compact is the
/// keyboard area and cannot host a field, §3.5). There is no player-count
/// picker any more: it was off-theme (a segmented `Picker` reads as glass, not
/// wood/wool) AND wrong, per the owner's own framing — "New game should just
/// stage the new game, lobby style, with unspecified player count until
/// someone hits start".
///
/// Lobby v3 (note 2) unified DM and group behind ONE path: "Create game"
/// always opens a lobby (LobbyView), never a straight-to-board deal — a DM
/// used to deal LIVE immediately here, which let the creator reroll a bad
/// hand by tapping New game until the deck favored them, since nothing
/// committed the seed until they'd already seen it. A DM's lobby capacity is
/// just 2 (`GameSurface.createWaiting`), so "Players: 2" is still shown as a
/// fact, not a picker — nobody has joined yet, and nobody needs to pick a
/// count: whoever has joined when someone taps Start (or Join and start) IS
/// the player count (§5.2/lobby v3).
private struct NewGameSetup: View {
    @State private var nickname: String
    let isDM: Bool
    /// No longer displayed (the picker it used to size is gone). Kept only so
    /// this struct's callers — this file's own call site, the harness, and
    /// MessagesViewController, all of which compute a real participant count —
    /// keep compiling unchanged (source compatibility, no Swift compiler here
    /// to re-check call sites across targets).
    let chatPlayers: Int
    let onStart: (String) -> Void

    init(nickname: String, isDM: Bool, chatPlayers: Int, onStart: @escaping (String) -> Void) {
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
        self.isDM = isDM
        self.chatPlayers = chatPlayers
        self.onStart = onStart
    }

    var body: some View {
        VStack(spacing: 16) {
            Text(FStrings.t("ios.msg.newgame")).font(.headline).foregroundStyle(FColor.ink)
            VStack(alignment: .leading, spacing: 4) {
                Text(FStrings.t("ios.msg.yourname")).font(.footnote).foregroundStyle(.black.opacity(0.55))
                TextField(FStrings.t("ios.you"), text: $nickname).textFieldStyle(.roundedBorder)
            }
            FButton(FStrings.t("ios.msg.creategame"), kind: .wood) {
                let n = nickname.trimmingCharacters(in: .whitespaces)
                onStart(n.isEmpty ? FStrings.t("ios.you") : n)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// The WAITING lobby, rewritten for lobby v3 (§5.2/§5.3, docs/IMESSAGE_LOBBY_V3.md,
/// notes 2/14/15/16): an OPEN lobby, not a fixed seat count. `env.nPlayers` is
/// the lobby's CAPACITY — 8 for a group (the wire's max) or 2 for a DM (see
/// `GameSurface.createWaiting`) — display convention only, never rendered as N
/// literal seats: the joined list IS the player count so far, and the game's
/// real size is decided at Start, not now.
///
/// What a viewer can do: Join (name + button, if I have not claimed a seat and
/// the lobby has room), or Start (once I'm already joined and 2+ have — any
/// joined player may, §5.2). Owner decision (this pass): there is NO combined
/// "Join and start" button — a joiner either Joins (which stages the WAITING
/// lobby for the human to send) or, being already joined, taps Start (which
/// stages the LIVE game). Two distinct texts, never one fused action. There is
/// likewise no "Send invite" button (notes 14/16): creating and joining both
/// AUTO-STAGE the reseal, so the human's very next tap is Messages' own Send.
/// WHICH control a lobby offers, pulled out of `LobbyView` as a pure function
/// for one reason: the view had a state with NO control at all — joined, but
/// fewer than two players, so no Start (needs 2), no Join (already in), no
/// invite (notes 14/16 removed that button as redundant with the auto-stage).
/// A lobby listing one player and offering nothing is a dead end, and it is
/// invisible in a `if/else if/else` chain until someone lands in it. As an enum
/// there is always exactly one answer, and a test can enumerate every
/// (mySeat, joined, capacity) and assert so.
public enum LobbyControls {
    /// Start the game at the joined count (I'm in, 2+ have joined).
    case start
    /// Re-stage the WAITING chain so the human can send the invite (I'm in,
    /// nobody else is yet, and the newest invite is NOT mine).
    case invite
    /// I'm in, nobody else is yet, and the invite sitting at the head of this
    /// chain is the one I put there. There is genuinely nothing to do but wait
    /// for someone to join, so the lobby says so and offers no button.
    case waiting
    /// Claim a seat (I'm not in, and there is room).
    case join
    /// Nothing to do but wait (I'm not in, and there is no room).
    case full

    /// `iSentTheInvite`: is the newest bubble on this chain one I staged or
    /// sent (`lastActorSeat == mySeat`)? Round-4 note 1 — "if you were the last
    /// one to send an invite, shouldn't have the Send invite pop up." Offering
    /// it then asks the human to send a second copy of the invite already
    /// sitting in the thread (or in the compose field, freshly auto-staged),
    /// which is the state a creator lands in every single time.
    ///
    /// The trade this makes, deliberately and with the owner's call on it: the
    /// `.invite` button exists as the recovery path for a lobby whose
    /// auto-staged bubble is gone (sent, deleted from the compose field, or
    /// the extension reopened later). Gating it on authorship means a creator
    /// who deletes their own draft has no in-lobby way to re-stage it and must
    /// use New game. That is the cost of not nagging everyone else.
    public static func offered(mySeat: Int?, joined: Int, capacity: Int,
                               iSentTheInvite: Bool = false) -> LobbyControls {
        if mySeat != nil {
            if joined >= 2 { return .start }
            return iSentTheInvite ? .waiting : .invite
        }
        return joined < capacity ? .join : .full
    }
}

private struct LobbyView: View {
    let env: MessageEnvelope
    let mySeat: Int?
    let onJoin: (String) -> Void
    let onStart: () -> Void
    /// Re-stage this same WAITING chain so the human can send the invite again
    /// — the recovery path for a lobby whose auto-staged bubble is gone (see
    /// the `mySeat != nil, joins < 2` branch in `body`).
    let onInvite: () -> Void

    /// The joiner's editable name (B3): compact can't host a field, so this is the
    /// place a joiner names themselves before claiming a seat. Seeded from the
    /// stored nickname, blank if it's the neutral default.
    @State private var nickname: String

    init(env: MessageEnvelope, mySeat: Int?, nickname: String,
         onJoin: @escaping (String) -> Void,
         onStart: @escaping () -> Void,
         onInvite: @escaping () -> Void) {
        self.env = env; self.mySeat = mySeat
        self.onJoin = onJoin; self.onStart = onStart; self.onInvite = onInvite
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
    }

    var body: some View {
        VStack(spacing: 12) {
            Text(FStrings.t("ios.lobby")).font(.headline).foregroundStyle(FColor.ink)
            // Joined players only — never env.nPlayers rows: an open lobby has
            // no "open seat" placeholders, because there is no fixed seat count
            // to fill (note 19/25's whole point, unchanged by v3).
            VStack(spacing: 6) {
                ForEach(env.joins.sorted { $0.seat < $1.seat }, id: \.seat) { j in
                    HStack {
                        Text("\(j.seat + 1).").foregroundStyle(.black.opacity(0.55)).monospacedDigit()
                        Text(j.name + (j.seat == mySeat ? " (\(FStrings.t("ios.you")))" : ""))
                            .foregroundStyle(FColor.ink)
                        Spacer()
                    }
                }
            }
            .padding(.horizontal)

            // note 16: no "Waiting for players — N joined" line here any more —
            // the joined list above already says exactly that, and the owner's
            // read was "the lobby is too tight" for a second line saying the
            // same thing.
            switch LobbyControls.offered(mySeat: mySeat, joined: env.joins.count,
                                         capacity: env.nPlayers,
                                         iSentTheInvite: env.lastActorSeat == mySeat) {
            case .start:
                FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
            case .waiting:
                // Round-4 note 1: my own invite is the newest thing on this
                // chain, so there is nothing to send that isn't already sent.
                Text(FStrings.t("ios.msg.waiting"))
                    .font(.footnote).foregroundStyle(.black.opacity(0.55))
            case .invite:
                    // I'm in, nobody else is yet. This branch used to render
                    // NOTHING — no Start (needs 2), no Join (I'm joined), no
                    // invite (notes 14/16 dropped that button as redundant with
                    // the auto-stage). Which is a dead end the moment the
                    // auto-staged invite is gone: sent already, or deleted from
                    // the input field, or the extension reopened later. The
                    // owner hit exactly that — a lobby listing one player and
                    // not a single control on it.
                    //
                    // The invite button is only redundant while the auto-staged
                    // bubble is still sitting in the compose field, so it comes
                    // back HERE and only here: re-stage the same WAITING chain
                    // so there is always a way to ask someone to join.
                    Text(FStrings.t("ios.msg.waiting"))
                        .font(.footnote).foregroundStyle(.black.opacity(0.55))
                    FButton(FStrings.t("ios.msg.invite"), kind: .wood, action: onInvite)
            case .join:
                // Same width as the buttons below (note 29) — both rely on the
                // outer .padding() alone, no extra inset on the field.
                TextField(FStrings.t("ios.you"), text: $nickname).textFieldStyle(.roundedBorder)
                FButton(FStrings.t("ios.msg.joinas", ["name": displayName]), kind: .wood) { onJoin(nickname) }
            case .full:
                Text(FStrings.t("ios.msg.lobbyfull")).font(.footnote).foregroundStyle(.black.opacity(0.55))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private var displayName: String {
        let t = nickname.trimmingCharacters(in: .whitespaces)
        return t.isEmpty ? FStrings.t("ios.you") : t
    }
}

/// §B3 one-time name entry for a player being seated without a stored name — the
/// 2-player receiver, who has neither the creator's setup screen nor the 3-8p
/// lobby's join field. Shown once (until a name is stored), prefilled with the
/// current nickname if it is not the neutral default. Continue is always enabled:
/// an empty field just means "call me the default", which still counts as chosen.
private struct NameGateView: View {
    @State private var name: String
    let onContinue: (String) -> Void

    init(prefill: String, onContinue: @escaping (String) -> Void) {
        _name = State(initialValue: prefill == "Me" ? "" : prefill)
        self.onContinue = onContinue
    }

    var body: some View {
        VStack(spacing: 16) {
            Text(FStrings.t("ios.msg.nameprompt")).font(.headline)
                .foregroundStyle(FColor.ink).multilineTextAlignment(.center)
            // No extra .padding(.horizontal) here — the field and the button below
            // both rely solely on the VStack's outer .padding() so they render the
            // same width (note 29; the field used to be inset twice, making it
            // visibly narrower than the full-width Continue button).
            TextField(FStrings.t("ios.you"), text: $name).textFieldStyle(.roundedBorder)
                .submitLabel(.done).onSubmit { onContinue(name) }
            FButton(FStrings.t("ios.msg.continue"), kind: .wood) { onContinue(name) }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// §6.3 tertiary identity: N≥3, cache lost, not the last actor - ask the human.
/// Offers every seat (named where a join is known, else "Seat N") so it also
/// covers the DEBUG single-sim case, where a 2-player game has only one join.
private struct SeatPicker: View {
    let nPlayers: Int
    let joins: [MessageJoin]
    let onPick: (Int) -> Void

    private func label(_ seat: Int) -> String {
        joins.first { $0.seat == seat }?.name ?? "Seat \(seat + 1)"
    }

    var body: some View {
        VStack(spacing: 12) {
            Text(FStrings.t("ios.msg.pickseat")).font(.headline).foregroundStyle(FColor.ink)
            ForEach(0..<nPlayers, id: \.self) { seat in
                FButton(label(seat), kind: .secondary) { onPick(seat) }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

#if DEBUG
/// DEBUG-only knobs for single-simulator testing (never compiled into Release).
public enum MessageDebugFlags {
    /// Force the seat picker on every adopted bubble so both seats are playable
    /// on ONE simulator (which cannot otherwise distinguish sender from receiver).
    /// The FoolishHarness turns this OFF: it gives each fake participant a
    /// distinct identity + its own seat cache, so seat inference resolves
    /// automatically and the picker would be wrong to show.
    public static var pickSeatOnAdopt = true
}
#endif


private struct DamagedView: View {
    var body: some View {
        VStack(spacing: 8) {
            Text("Foolish").font(.headline).foregroundStyle(FColor.ink)
            Text(FStrings.t("ios.msg.damaged")).font(.footnote).foregroundStyle(.black.opacity(0.55))
                .multilineTextAlignment(.center).padding(.horizontal)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
