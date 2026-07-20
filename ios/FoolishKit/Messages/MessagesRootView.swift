// MessagesRootView — what the extension shows, per presentation style (§10).
//
// Compact is the KEYBOARD AREA (§3.5): no text field, no horizontal scrollers —
// so it is a label and buttons. Expanded is the table. The routing here is the
// §5/§6/§7 machine wearing a UI: a selected bubble is decoded + adopted, my seat
// is resolved (§6), and I either play (MessageTableView, staging a reply) or,
// when three-plus players leave my seat ambiguous, pick who I am (§6.3). New game
// deals a fresh genesis game where I am seat 0 (§5.2).
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
                startNewGame: Bool, newGameToken: Int = 0, chatIsDM: Bool, chatPlayers: Int,
                requestExpand: @escaping () -> Void, onNewGame: @escaping () -> Void,
                onSend: @escaping (Data, Int) async -> Void,
                onUnstage: @escaping () -> Void = {}) {
        self.payloadURL = payloadURL; self.style = style; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.newGameToken = newGameToken
        self.chatIsDM = chatIsDM; self.chatPlayers = chatPlayers
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
                    startNewGame: startNewGame, newGameToken: newGameToken,
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
                              let survivors: [Move]; let discarded: Int }

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
    /// (newGameToken) changes it, which resets and reloads.
    private var loadKey: String { "\(newGameToken)|\(startNewGame)|\(payloadURL?.absoluteString ?? "")" }

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
            .task(id: loadKey) { await reloadForInput() }
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
                      onSend: { Task { await onSend(lob.payload, lobbySeat(lob.env) ?? 0) } },
                      onJoin: { name in Task { await joinLobby(lob, nickname: name) } })
        } else if let g = nameGate {
            NameGateView(prefill: MessageGameStore.shared.nickname) { name in
                Task { await nameThenSeat(name, gate: g) }
            }
        } else if showSetup {
            NewGameSetup(nickname: MessageGameStore.shared.nickname,
                         isDM: chatIsDM, chatPlayers: chatPlayers) { name, players in
                Task { await start(nickname: name, players: players) }
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
                    .font(.footnote).foregroundStyle(FColor.textDim)
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
        controller = nil; lobby = nil; nameGate = nil; showSetup = false
        ambiguous = nil; spectator = nil; damaged = false
        await load()
    }

    private func load() async {
        guard let url = payloadURL else {
            // No bubble is selected. If the human explicitly tapped New game
            // (startNewGame), the setup IS the screen. Otherwise this is a plain
            // re-open of the extension - and if we already committed a game (we
            // sent one, or adopted one), reopen THAT rather than offering New game
            // again (one game per chat: "I already sent a game, why New game?").
            if !startNewGame,
               let latest = MessageGameStore.shared.games().first,
               let payload = Base32.decode(latest.payloadBase32),
               let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1) {
                if env.phase == 0 { lobby = Lobby(env: env, payload: payload) }
                else { await adopt(winner: payload, env: env) }
                return
            }
            // Nothing to reopen (first-ever open, or an explicit New game).
            showSetup = true
            return
        }
        do {
            let incoming = try MessageEnvelope.payloadBytes(url: url)
            // Decode ADOPTS and VALIDATES — a damaged link throws here (§7.3).
            let env = try await MessageEnvelope.decode(payload: incoming, viewer: -1)

            // A WAITING bubble is a lobby, not a board — and Rule P's play-time
            // staleness does not apply (every lobby bubble is round 0/turn 0). Show
            // the seats and the join button (§5.2).
            if env.phase == 0 {
                lobby = Lobby(env: env, payload: incoming)
                return
            }

            // Rule P (§7.2): if the chain we already hold strictly out-ranks the
            // tapped bubble (a later state that arrived out of order, or a newer
            // chain we committed), just open OURS — the canonically-latest state.
            // One game per chat, so there is no "this game has moved on / open the
            // latest / view anyway" prompt: we silently adopt whichever chain wins
            // Rule P. Delivery order is never trusted; only the bytes decide.
            if let row = MessageGameStore.shared.record(gameId: env.gameId),
               let preferred = Base32.decode(row.payloadBase32), preferred != incoming,
               ((try? await MessageKernel.shared.preferred(preferred, incoming)) ?? 0) < 0,
               let penv = try? await MessageEnvelope.decode(payload: preferred, viewer: -1) {
                await adopt(winner: preferred, env: penv)
                return
            }

            // The tapped chain wins, ties, or is the first we've seen: adopt it.
            await adopt(winner: incoming, env: env)
        } catch {
            damaged = true
        }
    }

    // MARK: creation + lobby (§5.2)

    /// Finish the New game setup: persist the nickname (B3), then either deal a
    /// 2-player LIVE game straight to the board, or open an N>=3 WAITING lobby.
    private func start(nickname: String, players: Int) async {
        MessageGameStore.shared.nickname = nickname
        showSetup = false
        if players == 2 { await startGenesis(nickname: nickname) }
        else { await createWaiting(players: players, nickname: nickname) }
    }

    /// Create an N>=3 game as seat 0 and open its lobby: fix the seed + player
    /// count in the kernel, seal a WAITING bubble seating only me, and cache my
    /// seat so a later open resolves it (§6.1).
    private func createWaiting(players: Int, nickname: String) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: players)
            let joins = [MessageJoin(seat: 0, name: nickname)]
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: 0, env: env, payload: payload)
            lobby = Lobby(env: env, payload: payload)
        } catch {
            damaged = true
        }
    }

    /// My seat in a lobby, or nil if I have not claimed one yet (§6). The creator
    /// and any joiner who already sent a claim resolve to a seat; a fresh joiner is
    /// nil, which is what shows the Join button.
    private func lobbySeat(_ env: MessageEnvelope) -> Int? {
        switch SeatIdentity.resolve(cachedSeat: MessageGameStore.shared.seat(gameId: env.gameId),
                                    senderIsLocal: senderIsLocal,
                                    nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat) {
        case .known(let s): return s
        case .ambiguous:    return nil
        }
    }

    /// Claim the lowest free seat (§5.2). While seats remain, reseal WAITING and
    /// stay in the lobby; the claim that fills the LAST seat seals a LIVE handoff
    /// ("game on") and drops into the board. Either way the human presses Send.
    private func joinLobby(_ lob: Lobby, nickname: String) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        let trimmed = nickname.trimmingCharacters(in: .whitespaces)
        let nick = trimmed.isEmpty ? FStrings.t("ios.you") : trimmed
        MessageGameStore.shared.nickname = nick   // remember it for the next game (B3)
        let joins = (env.joins + [MessageJoin(seat: free, name: nick)]).sorted { $0.seat < $1.seat }
        let full = joins.count == env.nPlayers
        do {
            // Re-adopt the lobby so the seed + player count are resident for the seal.
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: full ? 2 : 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: free, env: newEnv, payload: payload)
            await onSend(payload, free)
            if full {
                controller = MessageTurnController(parentPayload: payload, parent: newEnv, mySeat: free)
                lobby = nil
            } else {
                lobby = Lobby(env: newEnv, payload: payload)
            }
        } catch {
            damaged = true
        }
    }

    /// Adopt `winner` as the game, rebase my staged-but-unsent moves onto it
    /// (Rule R, §7.4), refresh the preferred-chain cache, and open the board.
    private func adopt(winner: Data, env: MessageEnvelope) async {
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

        switch SeatIdentity.resolve(cachedSeat: MessageGameStore.shared.seat(gameId: env.gameId),
                                    senderIsLocal: senderIsLocal,
                                    nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat) {
        case .known(let seat):
            // §B3: a player about to be seated who has never chosen a name is asked
            // once (the 2-player receiver has no setup/lobby screen). Creator +
            // lobby joiners already set theirs, so this only fires for the DM
            // opponent's first game; it never re-asks once stored.
            if !MessageGameStore.shared.hasSetNickname {
                nameGate = NameGate(env: env, payload: winner, seat: seat,
                                    survivors: survivors, discarded: discarded)
            } else {
                seatOnBoard(seat: seat, env: env, winner: winner,
                            survivors: survivors, discarded: discarded)
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
                             survivors: [Move], discarded: Int) {
        cache(seat: seat, env: env, payload: winner)
        toast = rebaseToast(survivors: survivors, discarded: discarded)
        controller = MessageTurnController(parentPayload: winner, parent: env, mySeat: seat,
                                           preStaged: survivors)
    }

    /// The human answered the name gate: persist the name (blank falls back to the
    /// neutral default, which still counts as "set" so we never re-ask), then seat
    /// them. The name is baked into `joins` when they first play (sealJoins).
    private func nameThenSeat(_ raw: String, gate g: NameGate) async {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        MessageGameStore.shared.nickname = trimmed.isEmpty ? FStrings.t("ios.you") : trimmed
        nameGate = nil
        seatOnBoard(seat: g.seat, env: g.env, winner: g.payload,
                    survivors: g.survivors, discarded: g.discarded)
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


    /// §6.3 pick resolved: remember the seat, rebase, then play.
    private func choose(seat: Int, from a: (env: MessageEnvelope, payload: Data)) async {
        cache(seat: seat, env: a.env, payload: a.payload)
        let (survivors, _) = await rebasePending(gameId: a.env.gameId, adoptedRound: a.env.round)
        controller = MessageTurnController(parentPayload: a.payload, parent: a.env, mySeat: seat,
                                           preStaged: survivors)
        ambiguous = nil
    }

    /// A 2-player DM game deals LIVE immediately (§5.2): no lobby, the creator is
    /// seat 0 and may play their first move before staging.
    private func startGenesis(nickname: String) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        controller = MessageTurnController(genesisSeed: seed, players: 2, gameId: gameId,
                                           myNickname: nickname)
    }

    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        MessageGameStore.shared.put(MessageGameRecord(
            gameId: env.gameId, mySeat: seat, nPlayers: env.nPlayers, round: env.round,
            turn: env.turn, phase: env.phase, finished: env.phase == 3, names: names,
            payloadBase32: Base32.encode(payload), updatedAt: Date().timeIntervalSince1970))
    }
}

/// New game setup (§5.2): the creator names themselves (B3 — the one place a
/// nickname is entered; compact is the keyboard area and cannot host a field,
/// §3.5) and picks a player count. Two players start a DM game at once; three or
/// more open a lobby others join. The wire allows 2-8.
///
/// Chat-aware (B4 feedback): a 1:1 DM can only be 2 players, so the picker is
/// hidden and locked to 2. A group chat defaults to its own participant count
/// but still lets the creator pick anything 2-8 (bots or a subset can fill it).
private struct NewGameSetup: View {
    @State private var nickname: String
    @State private var players: Int
    let isDM: Bool
    let onStart: (String, Int) -> Void

    init(nickname: String, isDM: Bool, chatPlayers: Int, onStart: @escaping (String, Int) -> Void) {
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
        _players = State(initialValue: isDM ? 2 : min(max(chatPlayers, 2), 8))
        self.isDM = isDM
        self.onStart = onStart
    }

    var body: some View {
        VStack(spacing: 16) {
            Text(FStrings.t("ios.msg.newgame")).font(.headline).foregroundStyle(FColor.textPrimary)
            VStack(alignment: .leading, spacing: 4) {
                Text(FStrings.t("ios.msg.yourname")).font(.footnote).foregroundStyle(FColor.textDim)
                TextField(FStrings.t("ios.you"), text: $nickname).textFieldStyle(.roundedBorder)
            }
            if isDM {
                Text(FStrings.t("players") + ": 2").font(.footnote).foregroundStyle(FColor.textDim)
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    Text(FStrings.t("players")).font(.footnote).foregroundStyle(FColor.textDim)
                    Picker(FStrings.t("players"), selection: $players) {
                        ForEach(2...8, id: \.self) { Text("\($0)").tag($0) }
                    }.pickerStyle(.segmented)
                }
            }
            FButton(FStrings.t("start_game"), kind: .wood) {
                let n = nickname.trimmingCharacters(in: .whitespaces)
                onStart(n.isEmpty ? FStrings.t("ios.you") : n, isDM ? 2 : players)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// The WAITING lobby (§5.2/§5.3): claimed seats by nickname, open seats, and the
/// one action that matters right now - Join if I have not claimed a seat and one
/// is free, else Send to stage the invite/handoff for the human to send.
private struct LobbyView: View {
    let env: MessageEnvelope
    let mySeat: Int?
    let onSend: () -> Void
    let onJoin: (String) -> Void

    /// The joiner's editable name (B3): compact can't host a field, so this is the
    /// place a joiner names themselves before claiming a seat. Seeded from the
    /// stored nickname, blank if it's the neutral default.
    @State private var nickname: String

    init(env: MessageEnvelope, mySeat: Int?, nickname: String,
         onSend: @escaping () -> Void, onJoin: @escaping (String) -> Void) {
        self.env = env; self.mySeat = mySeat; self.onSend = onSend; self.onJoin = onJoin
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
    }

    private var freeSeats: Int { env.nPlayers - env.joins.count }
    private func name(_ s: Int) -> String? { env.joins.first { $0.seat == s }?.name }

    var body: some View {
        VStack(spacing: 12) {
            Text(FStrings.t("ios.lobby")).font(.headline).foregroundStyle(FColor.textPrimary)
            VStack(spacing: 6) {
                ForEach(0..<env.nPlayers, id: \.self) { s in
                    HStack {
                        Text("\(s + 1).").foregroundStyle(FColor.textDim).monospacedDigit()
                        if let nm = name(s) {
                            Text(nm + (s == mySeat ? " (\(FStrings.t("ios.you")))" : ""))
                                .foregroundStyle(FColor.textPrimary)
                        } else {
                            Text(FStrings.t("ios.msg.seatopen")).foregroundStyle(FColor.textDim).italic()
                        }
                        Spacer()
                    }
                }
            }
            .padding(.horizontal)

            if mySeat != nil {
                Text(freeSeats > 0 ? FStrings.t("ios.msg.waitingjoin", ["n": "\(freeSeats)"])
                                   : FStrings.t("ios.msg.lobbyfull"))
                    .font(.footnote).foregroundStyle(FColor.textDim)
                FButton(FStrings.t("ios.msg.sendinvite"), kind: .wood, action: onSend)
            } else if freeSeats > 0 {
                TextField(FStrings.t("ios.you"), text: $nickname).textFieldStyle(.roundedBorder)
                    .padding(.horizontal)
                FButton(FStrings.t("ios.msg.joinas", ["name": displayName]), kind: .wood) { onJoin(nickname) }
            } else {
                Text(FStrings.t("ios.msg.lobbyfull")).font(.footnote).foregroundStyle(FColor.textDim)
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
                .foregroundStyle(FColor.textPrimary).multilineTextAlignment(.center)
            TextField(FStrings.t("ios.you"), text: $name).textFieldStyle(.roundedBorder)
                .submitLabel(.done).onSubmit { onContinue(name) }
                .padding(.horizontal)
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
            Text(FStrings.t("ios.msg.pickseat")).font(.headline).foregroundStyle(FColor.textPrimary)
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
            Text("Foolish").font(.headline).foregroundStyle(FColor.textPrimary)
            Text(FStrings.t("ios.msg.damaged")).font(.footnote).foregroundStyle(FColor.textDim)
                .multilineTextAlignment(.center).padding(.horizontal)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
