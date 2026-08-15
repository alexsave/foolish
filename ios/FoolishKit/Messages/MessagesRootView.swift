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
    /// Bumped by the host (MessagesViewController.didStartSending) each time the
    /// human actually SENDS a staged bubble. Threaded down so the live board's
    /// controller can drop the just-sent move from its in-memory pending
    /// (`markSent`) - otherwise the Undo button lingers in the collapsed view and
    /// re-stages an already-sent move (round-6 bug 4). Unlike newGameToken it must
    /// NOT change loadKey: a send re-presents the SAME game, so the board is
    /// signalled (via .onChange) rather than reloaded.
    let sentToken: Int
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
                startNewGame: Bool, newGameToken: Int = 0, sentToken: Int = 0, chatKey: String,
                chatIsDM: Bool, chatPlayers: Int,
                requestExpand: @escaping () -> Void, onNewGame: @escaping () -> Void,
                onSend: @escaping (Data, Int) async -> Void,
                onUnstage: @escaping () -> Void = {}) {
        self.payloadURL = payloadURL; self.style = style; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.newGameToken = newGameToken; self.sentToken = sentToken
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
        // The wool is a `.background` on the content — NOT a ZStack sibling. As a
        // sibling, `WoolBackground().ignoresSafeArea()` expands the stack into the
        // safe areas and `GameSurface` (maxHeight: .infinity) fills THAT taller
        // box, so the hand fan dropped off the bottom edge (cards "barely fit", cut
        // off). As a background the wool extends behind, into the safe area via its
        // own `.ignoresSafeArea()`, WITHOUT changing GameSurface's frame — so the
        // content keeps the safe-area height the hand was laid out against and the
        // wool still paints the whole screen. The "wool too short vertically" that
        // remained was the WEAVE IMAGE itself being a fixed size shorter than a
        // tall expanded surface (WoolWeave), fixed there, not here.
        GameSurface(payloadURL: payloadURL, style: style, senderIsLocal: senderIsLocal,
                    startNewGame: startNewGame, newGameToken: newGameToken, sentToken: sentToken,
                    chatKey: chatKey, chatIsDM: chatIsDM, chatPlayers: chatPlayers,
                    requestExpand: requestExpand, onNewGame: onNewGame, onSend: onSend,
                    onUnstage: onUnstage)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // Order matters: apply the wool FIRST, THEN ignore the keyboard on the
            // composite. That way the keyboard opt-out extends the CONTENT and the
            // WOOL together into the bottom/keyboard region, so the hand never sits
            // over a strip the wool didn't reach (the "background gap at the bottom"
            // seen after send-brings-up-the-keyboard, then reopening the bubble).
            // The old order ignored the keyboard on GameSurface alone, so the hand
            // extended down but the wool behind it did not.
            .background(WoolBackground())
            .ignoresSafeArea(.keyboard)
            // Round-5 M4/B3/M3: Dynamic Type had no POLICY at all — some
            // controls never scaled (M4), the card faces scaled straight out
            // of their own bounds (B3), and the game-over list collapsed
            // independently of both (M3). Owner's call this round: opt OUT of
            // Dynamic Type entirely rather than pick apart which of dozens of
            // small-screen surfaces can safely grow — "make a clamp so that
            // dynamic type does nothing in my game." The single-value overload
            // (not a range) pins the WHOLE hierarchy below this line to the
            // default, non-accessibility size regardless of the system
            // setting. Revisit if/when there is room to do this surface by
            // surface instead of as one blanket clamp.
            .dynamicTypeSize(.large)
    }
}

private struct GameSurface: View {
    let payloadURL: URL?
    let style: MsgPresentation
    let senderIsLocal: Bool
    let startNewGame: Bool
    let newGameToken: Int
    let sentToken: Int
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
            // Round-6 bug 4: the human just SENT the staged bubble (the host bumped
            // `sentToken` from didStartSending). Tell the live controller its move
            // is now in the thread so it drops it from `pending` - `canSend`/
            // `canUndo` go false and the collapsed drawer's Undo button, which
            // otherwise lingered and re-staged an already-sent move, disappears.
            // `sentToken` is deliberately absent from loadKey, so this fires WITHOUT
            // reloading the surface (the game is unchanged, only its staged move is
            // no longer pending).
            .onChange(of: sentToken) { _ in
                Task { await controller?.markSent() }
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
                      onInvite: { Task { await onSend(lob.payload, lobbySeat(lob.env) ?? 0) } },
                      // nil in every shipping build: the closure only exists
                      // where `addSoloSeat` is compiled at all.
                      onAddSoloSeat: soloSeatAction(lob))
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
                // Round-5 M10: full-opacity ink + a LIGHT shadow, not 55%
                // black — the busy wool weave has no fixed-opacity foreground
                // that survives it (see the sweep note on DamagedView below).
                // Round-6 #17 added the weight: `onWoolText` (Tokens.swift).
                Text(FStrings.t("ios.msg.spectating"))
                    .font(.footnote).onWoolText()
                    .multilineTextAlignment(.center).padding(.horizontal).padding(.bottom, 8)
            }
        } else if damaged {
            DamagedView(onNewGame: onNewGame)
        } else {
            // 1.0(4) live-receive blink: while a received bubble reloads the
            // surface (controller briefly nil), a ProgressView spinner flashed
            // over the wool for a frame or two - the "slight blink". The reload is
            // sub-frame in the common case, so show the steady wool (Color.clear
            // over GameSurface's WoolBackground) instead of a spinner that
            // announces the reload. NOTE: the board still tears down and remounts
            // on a live receive (that remount is what drives the incoming-move
            // replay off the view nil->value transition); removing the remount
            // entirely needs frame-by-frame harness verification, tracked
            // separately, since it would otherwise kill that replay.
            Color.clear
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
            // Calls startGame() directly — bypasses LobbyControls.offered's
            // round-5 M9 gate (that gate only governs the UI's Start
            // button). Fine here: this is a scripted driver racing to a
            // dealt board for a screenshot, not a human who could be locked
            // out of one.
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
        // Round-5 B1: NewGameSetup only calls this from its `.ok` branch, so
        // `nickname` is already NicknameGate-valid and trimmed — the "You"
        // fallback that used to live here is unreachable now. Re-check
        // defensively anyway (never trust a caller's promise past the type
        // system) and, per M2, fall back to the STORED nickname rather than
        // a placeholder if it somehow is not: skipping the write below just
        // leaves whatever this device already had on file.
        if case .ok(let name) = NicknameGate.check(nickname) {
            MessageGameStore.shared.nickname = name
        }
        showSetup = false
        await createWaiting(nickname: MessageGameStore.shared.nickname)
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
        // Round-5 B1: LobbyView's join button is only reachable from its
        // `.ok` branch, so `nickname` is already NicknameGate-valid and
        // trimmed — the "You" fallback that used to live here is unreachable
        // now. Re-check defensively anyway and, per M2, fall back to the
        // STORED nickname (never a placeholder) if it somehow is not.
        let nick: String
        if case .ok(let name) = NicknameGate.check(nickname) {
            nick = name
        } else {
            nick = MessageGameStore.shared.nickname
        }
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

    /// The lobby's "Add player" action, or nil when solo seating is not
    /// compiled in — one `#if` here instead of one at the call site, so the
    /// view code above reads the same in every configuration.
    private func soloSeatAction(_ lob: Lobby) -> (() -> Void)? {
        #if DEBUG || SOLO_TESTING
        return { Task { await addSoloSeat(lob) } }
        #else
        return nil
        #endif
    }

    #if DEBUG || SOLO_TESTING
    /// Testing-only (MessageDebugFlags.soloSeats): seat a PUPPET player from this
    /// device so a lobby with nobody else in the chat can still reach two seats
    /// and start. Mechanically `joinLobby` minus the two things that would be
    /// wrong here:
    ///
    ///   - it does NOT overwrite this device's stored nickname (the puppet is
    ///     not me renaming myself — I keep my own name on my own seat), and
    ///   - it does NOT re-cache MY seat as the puppet's, so identity stays
    ///     whatever it already was; `pickSeatOnAdopt` is what switches which
    ///     hand you are playing, one bubble at a time.
    ///
    /// It also deliberately does not stage/send the reseal: a puppet is local
    /// scaffolding, and `startGame` seals the LIVE handoff off this same
    /// in-memory lobby payload, so the chat only ever sees the real game.
    private func addSoloSeat(_ lob: Lobby) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        let keepSeat = lobbySeat(env) ?? 0
        let joins = (env.joins + [MessageJoin(seat: free, name: "Solo \(free + 1)")])
            .sorted { $0.seat < $1.seat }
        do {
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: keepSeat, env: newEnv, payload: payload)
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }
    #endif

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
        // Round 7: `prevPayload` (the previously-cached chain) is gone — the
        // open-replay was already resolved purely from the adopted chain by the
        // kernel (MessageTurnController.begin -> lastMoveEvents), never from a
        // cached diff, so there is nothing to look up here any more.
        let prevPayload: Data? = nil
        // Make the resident game the winner (the round guard/ledger it used to set
        // are gone with Rule R).
        _ = try? await MessageKernel.shared.decode(payload: winner, viewer: -1)
        #if DEBUG || SOLO_TESTING
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
            #if DEBUG || SOLO_TESTING
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

    /// The human answered the name gate: persist the name, then seat them. The
    /// name is baked into `joins` when they first play (sealJoins). Round-5
    /// B1: NameGateView's Continue/onSubmit are only reachable from their
    /// `.ok` branch, so `raw` is already NicknameGate-valid and trimmed — the
    /// "call me the default" blank fallback this used to have is gone (see
    /// NameGateView's own doc). Re-check defensively anyway and, per M2, fall
    /// back to the STORED nickname (never a placeholder) if it somehow is not
    /// — skipping the write below just leaves whatever this device already
    /// had on file.
    private func nameThenSeat(_ raw: String, gate g: NameGate) async {
        if case .ok(let name) = NicknameGate.check(raw) {
            MessageGameStore.shared.nickname = name
        }
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

    /// Round 7: persist ONLY this device's seat (§6.1). The preferred-chain
    /// payload, denormalized display fields and pending ledger the old record
    /// carried are gone — the extension always renders the tapped bubble now, so
    /// the one thing worth keeping is which seat is me in this game.
    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        MessageGameStore.shared.setSeat(gameId: env.gameId, chatKey: chatKey, seat: seat)
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

    /// Round-5 B1: the three-state verdict on the CURRENT field text, driving
    /// both the Create-game button's label/enabled state and — via `.ok` —
    /// the exact trimmed name `onStart` is called with. A name that fails
    /// either of NicknameGate's caps is REJECTED here, in the UI, rather than
    /// lighting the button up and failing downstream at the seal layer as
    /// "this game link is damaged" (B1's actual bug).
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onWoolText` (Tokens.swift) is the wool half of
            // the wood/wool text pairing, thickened per the owner's ask.
            Text(FStrings.t("ios.msg.newgame")).font(.headline).onWoolText()
            // Round-7 #1: the "Your name" label is dropped - the field's own
            // "your nickname" placeholder already says what it is, and the two
            // together were redundant. The placeholder carries it alone now.
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname).textFieldStyle(.roundedBorder)
            switch nameVerdict {
            case .ok(let name):
                FButton(FStrings.t("ios.msg.creategame"), kind: .wood) { onStart(name) }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
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
/// the lobby has room), or Start (once I'm already joined and 2+ have —
/// round-5 M9 narrows "any joined player may" to "any joined player except
/// whoever sent the newest bubble, unless the lobby is full" — see
/// `LobbyControls.offered`). Owner decision (this pass): there is NO combined
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
    /// Start the game at the joined count. Round-5 M9 narrows this further —
    /// it is withheld from whoever sent the newest bubble while the lobby
    /// still has room (see `offered`'s doc): "2+ have joined" is no longer
    /// sufficient on its own.
    case start
    /// Re-stage the WAITING chain so the human can send the invite (I'm in,
    /// nobody else is yet, and the newest invite is NOT mine).
    case invite
    /// Nothing to do but wait. Two distinct situations render this way
    /// (round-5 M9 added the second one):
    ///  1. I'm in, nobody else is yet, and the invite sitting at the head of
    ///     this chain is the one I put there — unchanged from before M9.
    ///  2. I'm in, 2+ have joined, the lobby still has room, and the newest
    ///     bubble on the chain is mine (I just joined, or just re-staged the
    ///     invite) — M9's whole point: I cannot also be the one who starts.
    /// Both read the same "Waiting for the others" text; the owner explicitly
    /// ruled out a capacity/"N of M" line to tell them apart (M9 — "no
    /// capacity text, too confusing").
    case waiting
    /// Claim a seat (I'm not in, and there is room).
    case join
    /// Nothing to do but wait (I'm not in, and there is no room).
    case full

    /// `iSentTheInvite`: is the newest bubble on this chain one I staged or
    /// sent (`lastActorSeat == mySeat`)? Kept its round-4 name even though
    /// round-5 widens what it gates (below): it is public API and
    /// `Round4Tests.swift` already binds this exact argument label, so
    /// renaming it would break that file's BUILD, not just one of its
    /// assertions, over a docstring nicety.
    ///
    /// Round-4 note 1 — "if you were the last one to send an invite,
    /// shouldn't have the Send invite pop up." Offering it then asks the
    /// human to send a second copy of the invite already sitting in the
    /// thread (or in the compose field, freshly auto-staged), which is the
    /// state a creator lands in every single time.
    ///
    /// The trade this makes, deliberately and with the owner's call on it: the
    /// `.invite` button exists as the recovery path for a lobby whose
    /// auto-staged bubble is gone (sent, deleted from the compose field, or
    /// the extension reopened later). Gating it on authorship means a creator
    /// who deletes their own draft has no in-lobby way to re-stage it and must
    /// use New game. That is the cost of not nagging everyone else.
    ///
    /// Round-5 M9 — "if you were the last to send one of those join texts,
    /// you can't send a start text... that will make it a bit more difficult
    /// to lock people out." Extends the SAME authorship check to `.start`:
    /// once 2+ have joined, whoever sent the newest bubble (the last joiner,
    /// or whoever last re-staged the invite) is withheld from Start too, as
    /// long as the lobby still has room — so whoever is currently able to act
    /// is never the same person who could instead invite one more player in.
    ///
    /// EXEMPTION: a FULL lobby (`joined == capacity`) always offers Start to
    /// its last joiner regardless of authorship. Nobody else could join
    /// instead, so withholding Start there would just strand a full lobby
    /// with no way forward — and in a 2-player DM (capacity 2) it would force
    /// an extra, pointless round-trip into every single game: the joiner
    /// filling the last seat immediately starting is the designed "join and
    /// start" flow (note 2), not the lockout M9 is guarding against.
    public static func offered(mySeat: Int?, joined: Int, capacity: Int,
                               iSentTheInvite: Bool = false) -> LobbyControls {
        if mySeat != nil {
            if joined >= 2 {
                if iSentTheInvite && joined < capacity { return .waiting }
                return .start
            }
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
    /// Testing-only (MessageDebugFlags.soloSeats): seat a puppet player from
    /// this device. nil in every shipping build — see `soloControls`.
    var onAddSoloSeat: (() -> Void)?

    /// The joiner's editable name (B3): compact can't host a field, so this is the
    /// place a joiner names themselves before claiming a seat. Seeded from the
    /// stored nickname, blank if it's the neutral default.
    @State private var nickname: String

    init(env: MessageEnvelope, mySeat: Int?, nickname: String,
         onJoin: @escaping (String) -> Void,
         onStart: @escaping () -> Void,
         onInvite: @escaping () -> Void,
         onAddSoloSeat: (() -> Void)? = nil) {
        self.env = env; self.mySeat = mySeat
        self.onJoin = onJoin; self.onStart = onStart; self.onInvite = onInvite
        self.onAddSoloSeat = onAddSoloSeat
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
    }

    var body: some View {
        VStack(spacing: 12) {
            // Round-6 #17: `onWoolText` (Tokens.swift).
            Text(FStrings.t("ios.lobby")).font(.headline).onWoolText()
            // Joined players only — never env.nPlayers rows: an open lobby has
            // no "open seat" placeholders, because there is no fixed seat count
            // to fill (note 19/25's whole point, unchanged by v3).
            VStack(spacing: 6) {
                ForEach(env.joins.sorted { $0.seat < $1.seat }, id: \.seat) { j in
                    HStack {
                        // Round-5 M10: full-opacity ink + a light shadow, not
                        // 55% black (see DamagedView's sweep note). Round-6
                        // #17 thickened both columns, not just the seat number.
                        Text("\(j.seat + 1).").onWoolText().monospacedDigit()
                        Text(j.name + (j.seat == mySeat ? " (\(FStrings.t("ios.you")))" : ""))
                            .onWoolText()
                        Spacer()
                    }
                }
            }
            .padding(.horizontal)

            // Testing-only solo controls REPLACE the normal ones when they are
            // live, rather than sitting alongside them: the shipping lobby can
            // legitimately be offering "waiting" at the same moment solo play
            // wants to offer Start, and two contradictory controls on one
            // screen is worse than either. See `soloControls`.
            if let onAddSoloSeat, soloSeatsEnabled {
                soloControls(onAddSoloSeat)
            } else {
                standardControls
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    /// Testing-only (SOLO_TESTING / DEBUG): "Add player" until the lobby has
    /// enough seats, then Start. Deliberately bypasses `LobbyControls.offered`
    /// — specifically its round-5 M9 authorship gate, which withholds Start
    /// from whoever sent the newest bubble so one human cannot lock the others
    /// out of a lobby that still has room. Seating a puppet from this device
    /// makes me the newest sender every time, so that gate would make solo play
    /// impossible; and there is by definition nobody to lock out.
    @ViewBuilder
    private func soloControls(_ addSeat: @escaping () -> Void) -> some View {
        if env.joins.count < env.nPlayers {
            FButton("Add player (testing)", kind: .wood, action: addSeat)
        }
        if env.joins.count >= 2 {
            FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
        }
    }

    @ViewBuilder
    private var standardControls: some View {
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
                // Round-4 note 1 / round-5 M9: the newest thing on this chain
                // is mine — either my own invite (nobody else has joined yet)
                // or my own join/re-staged invite in a lobby that still has
                // room (M9) — so there is nothing to send, and no Start,
                // that isn't already mine to wait out. Round-5 M10:
                // full-opacity ink + a light shadow, not 55% black. Round-6
                // #17: `onWoolText` (Tokens.swift).
                Text(FStrings.t("ios.msg.waiting"))
                    .font(.footnote).onWoolText()
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
                    Text(FStrings.t("ios.msg.waiting"))    // round-5 M10 / round-6 #17: see .waiting above
                        .font(.footnote).onWoolText()
                    FButton(FStrings.t("ios.msg.invite"), kind: .wood, action: onInvite)
            case .join:
                // Same width as the buttons below (note 29) — both rely on the
                // outer .padding() alone, no extra inset on the field. Round-5
                // B1: same three-state nickname gate as NewGameSetup (see
                // `nameVerdict`) — "Join as {name}" only appears once the
                // field holds a valid, trimmed name.
                TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname).textFieldStyle(.roundedBorder)
                switch nameVerdict {
                case .ok(let name):
                    FButton(FStrings.t("ios.msg.joinas", ["name": name]), kind: .wood) { onJoin(name) }
                case .empty:
                    FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
                case .tooLong:
                    FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
                }
            case .full:
                // Round-5 M10: full-opacity ink + a light shadow, not 55%
                // black. Round-6 #17: `onWoolText` (Tokens.swift).
                Text(FStrings.t("ios.msg.lobbyfull")).font(.footnote).onWoolText()
            }
    }

    /// Is solo seating compiled in AND switched on? False in every shipping
    /// build — the flag type itself does not exist there, so this is the one
    /// place the condition is spelled and the call site stays readable.
    private var soloSeatsEnabled: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDebugFlags.soloSeats
        #else
        return false
        #endif
    }

    /// Round-5 B1: the three-state verdict on the CURRENT field text (see
    /// NicknameGate). Replaces the old `displayName`, which only ever
    /// substituted the "You" placeholder for a blank field — there is no
    /// substitute name any more, a name that fails either cap is rejected
    /// outright, not replaced.
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }
}

/// §B3 one-time name entry for a player being seated without a stored name — the
/// 2-player receiver, who has neither the creator's setup screen nor the 3-8p
/// lobby's join field (m8: this is the ONE of the three name-asking screens
/// that is not redundant with another — the DM opponent never sees the other
/// two, so it cannot simply be deleted in favor of them). Shown once (until a
/// name is stored), prefilled with the current nickname if it is not the
/// neutral default.
///
/// Round-5 B1: Continue is no longer always enabled. It gates on the SAME
/// NicknameGate verdict as NewGameSetup and LobbyView's join — blank or
/// over-cap dims the button and swaps its label for the reason. There is no
/// "call me the default" fallback any more: a name is REQUIRED, never
/// substituted, and `.onSubmit` (the keyboard's own Return key) respects the
/// same gate so it cannot hand a rejected name onward either.
private struct NameGateView: View {
    @State private var name: String
    let onContinue: (String) -> Void

    init(prefill: String, onContinue: @escaping (String) -> Void) {
        _name = State(initialValue: prefill == "Me" ? "" : prefill)
        self.onContinue = onContinue
    }

    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(name) }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onWoolText` (Tokens.swift).
            Text(FStrings.t("ios.msg.nameprompt")).font(.headline)
                .onWoolText().multilineTextAlignment(.center)
            // No extra .padding(.horizontal) here — the field and the button below
            // both rely solely on the VStack's outer .padding() so they render the
            // same width (note 29; the field used to be inset twice, making it
            // visibly narrower than the full-width Continue button).
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $name).textFieldStyle(.roundedBorder)
                .submitLabel(.done).onSubmit {
                    if case .ok(let trimmed) = nameVerdict { onContinue(trimmed) }
                }
            switch nameVerdict {
            case .ok(let trimmed):
                FButton(FStrings.t("ios.msg.continue"), kind: .wood) { onContinue(trimmed) }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
            }
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
            // Round-6 #17: `onWoolText` (Tokens.swift).
            Text(FStrings.t("ios.msg.pickseat")).font(.headline).onWoolText()
            ForEach(0..<nPlayers, id: \.self) { seat in
                FButton(label(seat), kind: .secondary) { onPick(seat) }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

#if DEBUG || SOLO_TESTING
/// DEBUG-only knobs for single-device testing (never compiled into Release —
/// `#if DEBUG || SOLO_TESTING` means a TestFlight/App Store build cannot contain
/// any of this — the shipping Release build defines neither condition; the
/// on-device testing build opts in with SWIFT_ACTIVE_COMPILATION_CONDITIONS,
/// and the CI Release build is what proves it).
public enum MessageDebugFlags {
    /// Force the seat picker on every adopted bubble so both seats are playable
    /// on ONE simulator (which cannot otherwise distinguish sender from receiver).
    /// The FoolishHarness turns this OFF: it gives each fake participant a
    /// distinct identity + its own seat cache, so seat inference resolves
    /// automatically and the picker would be wrong to show.
    public static var pickSeatOnAdopt = true

    /// SOLO PLAY (owner ask, device testing): seat extra players from this one
    /// device so a real chat can reach a startable game with nobody else in it.
    ///
    /// The shipping lobby is deliberately un-startable alone — Start needs 2+
    /// joined, and the only way to a second join is another human on another
    /// device. That is correct for the product and useless for testing the
    /// extension on a phone: you cannot reach a board at all, so none of the
    /// board work can be checked on device. With this on, the lobby offers
    /// "Add player" (claims the next free seat with a puppet name) and offers
    /// Start as soon as two seats are filled, bypassing the round-5 M9
    /// authorship gate — that gate exists to stop one human locking others
    /// out, and in solo play there is nobody to lock out.
    ///
    /// Pairs with `pickSeatOnAdopt`: add a puppet, start, then every time you
    /// open a bubble the picker asks which seat you are, so one person plays
    /// every hand in one chat.
    public static var soloSeats = true
}
#endif


/// Round-5 M1: "This game link is damaged" used to be a dead end with no
/// action on it at all (docs/APP_REVIEW_NOTES.md M1). Owner's fix — "just
/// throw in the 'create a new game' button back, which when pressed will
/// initialize a new lobby" — is the SAME New-game affordance every other
/// dead end in this file already offers, not a bespoke retry/dismiss flow.
///
/// The owner also asked to exclude whoever sent the damaged link from the
/// fresh lobby. Not implementable as asked: participant identities are
/// device-scoped and never travel in the payload (see SeatIdentity's header —
/// there is no "sender" field to read here, let alone exclude by). A fresh
/// lobby that everyone, including whoever sent the bad link, re-joins by
/// choice is the version of this fix that can actually be built.
private struct DamagedView: View {
    let onNewGame: () -> Void

    var body: some View {
        VStack(spacing: 8) {
            // Round-6 #17: `onWoolText` (Tokens.swift).
            Text("Foolish").font(.headline).onWoolText()
            // Round-5 M10: full-opacity ink + a light shadow, not 55% black —
            // the busy wool weave has no fixed-opacity foreground that
            // survives it (M10's fix, applied throughout this file, mirrors
            // the plank rank column's BONE text on WOOD, which uses a DARK
            // shadow; ink text on the lighter wool needs the inverse, a LIGHT
            // one). Round-6 #17 added the weight both treatments share.
            Text(FStrings.t("ios.msg.damaged")).font(.footnote).onWoolText()
                .multilineTextAlignment(.center).padding(.horizontal)
            FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}
