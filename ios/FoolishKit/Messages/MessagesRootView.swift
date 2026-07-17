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
    let chatIsDM: Bool
    let chatPlayers: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    let onSend: (Data, Int) async -> Void

    public init(payloadURL: URL?, style: MsgPresentation, senderIsLocal: Bool,
                startNewGame: Bool, chatIsDM: Bool, chatPlayers: Int,
                requestExpand: @escaping () -> Void, onNewGame: @escaping () -> Void,
                onSend: @escaping (Data, Int) async -> Void) {
        self.payloadURL = payloadURL; self.style = style; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.chatIsDM = chatIsDM; self.chatPlayers = chatPlayers
        self.requestExpand = requestExpand; self.onNewGame = onNewGame; self.onSend = onSend
    }

    public var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(WoolBackground())          // the table surface, not system white
    }

    @ViewBuilder private var content: some View {
        switch style {
        case .compact:
            CompactView(hasGame: payloadURL != nil, requestExpand: requestExpand, onNewGame: onNewGame)
        case .expanded:
            ExpandedView(payloadURL: payloadURL, senderIsLocal: senderIsLocal,
                         startNewGame: startNewGame, chatIsDM: chatIsDM, chatPlayers: chatPlayers,
                         onSend: onSend)
        }
    }
}

private struct CompactView: View {
    let hasGame: Bool
    let requestExpand: () -> Void
    let onNewGame: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Text(hasGame ? FStrings.t("ios.msg.thread") : "Foolish")
                .font(.headline).foregroundStyle(FColor.textPrimary)
            if hasGame {
                FButton(FStrings.t("ios.msg.open"), kind: .wood, action: requestExpand)
                FButton(FStrings.t("ios.msg.newgame"), kind: .secondary, action: onNewGame)
            } else {
                FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
            }
        }
        .padding(.horizontal, 32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct ExpandedView: View {
    let payloadURL: URL?
    let senderIsLocal: Bool
    let startNewGame: Bool
    let chatIsDM: Bool
    let chatPlayers: Int
    let onSend: (Data, Int) async -> Void

    /// A tapped bubble that LOST Rule P to the chain we already trust (§7.6): the
    /// human opened an older, collapsed bubble. We do not silently adopt it.
    private struct Stale { let incoming: Data; let env: MessageEnvelope; let preferred: Data }
    /// A phase-0/handoff lobby the extension shows instead of the board (§5.2).
    private struct Lobby { let env: MessageEnvelope; let payload: Data }

    @State private var controller: MessageTurnController?
    @State private var ambiguous: (env: MessageEnvelope, payload: Data)?
    @State private var stale: Stale?
    @State private var lobby: Lobby?
    @State private var showSetup = false
    @State private var toast: String?
    @State private var damaged = false

    var body: some View {
        Group {
            if let controller {
                MessageTableView(controller: controller,
                                 onSend: { payload in await onSend(payload, controller.mySeat) })
            } else if let lob = lobby {
                LobbyView(env: lob.env, mySeat: lobbySeat(lob.env),
                          nickname: MessageGameStore.shared.nickname,
                          onSend: { Task { await onSend(lob.payload, lobbySeat(lob.env) ?? 0) } },
                          onJoin: { name in Task { await joinLobby(lob, nickname: name) } })
            } else if showSetup {
                NewGameSetup(nickname: MessageGameStore.shared.nickname,
                             isDM: chatIsDM, chatPlayers: chatPlayers) { name, players in
                    Task { await start(nickname: name, players: players) }
                }
            } else if let s = stale {
                StaleBanner(onNewest: { Task { await openNewest(s) } },
                            onAnyway: { Task { await openAnyway(s) } })
            } else if let a = ambiguous {
                SeatPicker(nPlayers: a.env.nPlayers, joins: a.env.joins) { seat in
                    Task { await choose(seat: seat, from: a) }
                }
            } else if damaged {
                DamagedView()
            } else {
                ProgressView().task { await load() }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .fToast($toast)
    }

    private func load() async {
        if startNewGame { showSetup = true; return }
        guard let url = payloadURL else { damaged = true; return }
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

            // Rule P (§7.2): compare the tapped chain to the preferred one we have
            // cached. If ours strictly wins, this bubble is stale - banner, don't
            // adopt. Delivery order is never trusted; only the bytes decide.
            if let row = MessageGameStore.shared.record(gameId: env.gameId),
               let preferred = Base32.decode(row.payloadBase32), preferred != incoming,
               ((try? await MessageKernel.shared.preferred(preferred, incoming)) ?? 0) < 0 {
                stale = Stale(incoming: incoming, env: env, preferred: preferred)
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
            cache(seat: seat, env: env, payload: winner)
            toast = rebaseToast(survivors: survivors, discarded: discarded)
            controller = MessageTurnController(parentPayload: winner, parent: env, mySeat: seat,
                                               preStaged: survivors)
        case .ambiguous:
            ambiguous = (env, winner)
        }
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

    /// §7.6 "open the newest": adopt the chain we prefer instead of the stale one.
    private func openNewest(_ s: Stale) async {
        guard let env = try? await MessageEnvelope.decode(payload: s.preferred, viewer: -1) else {
            damaged = true; return
        }
        stale = nil
        await adopt(winner: s.preferred, env: env)
    }

    /// §7.6 "show that state anyway": read-only-ish view of the stale chain. We do
    /// NOT rebase onto it or let it clobber the preferred cache — it lost Rule P.
    private func openAnyway(_ s: Stale) async {
        _ = try? await MessageKernel.shared.decode(payload: s.incoming, viewer: -1)
        let seat = SeatIdentity.resolve(cachedSeat: MessageGameStore.shared.seat(gameId: s.env.gameId),
                                        senderIsLocal: senderIsLocal,
                                        nPlayers: s.env.nPlayers, lastActorSeat: s.env.lastActorSeat)
        stale = nil
        if case .known(let seat) = seat {
            controller = MessageTurnController(parentPayload: s.incoming, parent: s.env, mySeat: seat)
        } else {
            ambiguous = (s.env, s.incoming)
        }
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

/// §7.6: the human tapped an older, collapsed bubble. The game has moved on; we
/// offer the newest state we know, and — since a tap should never be a dead end —
/// a way to look at the tapped state anyway (read-only in spirit; a move made on
/// it would just lose Rule P).
private struct StaleBanner: View {
    let onNewest: () -> Void
    let onAnyway: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Text(FStrings.t("ios.msg.moved")).font(.headline).multilineTextAlignment(.center)
                .foregroundStyle(FColor.textPrimary)
            FButton(FStrings.t("ios.msg.opennewest"), kind: .wood, action: onNewest)
            FButton(FStrings.t("ios.msg.viewanyway"), kind: .secondary, action: onAnyway)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

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
