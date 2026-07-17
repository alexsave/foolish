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
import Messages
import FoolishKit

struct MessagesRootView: View {
    let payloadURL: URL?
    let style: MSMessagesAppPresentationStyle
    let senderIsLocal: Bool
    let startNewGame: Bool
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    let onSend: (Data, Int) async -> Void

    var body: some View {
        switch style {
        case .compact:
            CompactView(hasGame: payloadURL != nil, requestExpand: requestExpand, onNewGame: onNewGame)
        default:
            ExpandedView(payloadURL: payloadURL, senderIsLocal: senderIsLocal,
                         startNewGame: startNewGame, onSend: onSend)
        }
    }
}

private struct CompactView: View {
    let hasGame: Bool
    let requestExpand: () -> Void
    let onNewGame: () -> Void

    var body: some View {
        VStack(spacing: 10) {
            Text(hasGame ? FStrings.t("ios.msg.thread") : "Foolish").font(.headline)
            if hasGame {
                Button(FStrings.t("ios.msg.open"), action: requestExpand).buttonStyle(.borderedProminent)
                Button(FStrings.t("ios.msg.newgame"), action: onNewGame).buttonStyle(.bordered)
            } else {
                Button(FStrings.t("ios.msg.newgame"), action: onNewGame).buttonStyle(.borderedProminent)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct ExpandedView: View {
    let payloadURL: URL?
    let senderIsLocal: Bool
    let startNewGame: Bool
    let onSend: (Data, Int) async -> Void

    @State private var controller: MessageTurnController?
    @State private var ambiguous: (env: MessageEnvelope, payload: Data)?
    @State private var damaged = false

    var body: some View {
        Group {
            if let controller {
                MessageTableView(controller: controller,
                                 onSend: { payload in await onSend(payload, controller.mySeat) })
            } else if let a = ambiguous {
                SeatPicker(joins: a.env.joins) { seat in choose(seat: seat, from: a) }
            } else if damaged {
                DamagedView()
            } else {
                ProgressView().task { await load() }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func load() async {
        if startNewGame { await startGenesis(); return }
        guard let url = payloadURL else { damaged = true; return }
        do {
            let bytes = try MessageEnvelope.payloadBytes(url: url)
            // Decode ADOPTS and VALIDATES — a damaged link throws here (§7.3).
            let env = try await MessageEnvelope.decode(payload: bytes, viewer: -1)
            let cached = MessageGameStore.shared.seat(gameId: env.gameId)
            switch SeatIdentity.resolve(cachedSeat: cached, senderIsLocal: senderIsLocal,
                                        nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat) {
            case .known(let seat):
                controller = MessageTurnController(parentPayload: bytes, parent: env, mySeat: seat)
            case .ambiguous:
                ambiguous = (env, bytes)
            }
        } catch {
            damaged = true
        }
    }

    /// §6.3 pick resolved: remember the seat for next time, then play.
    private func choose(seat: Int, from a: (env: MessageEnvelope, payload: Data)) {
        cache(seat: seat, env: a.env, payload: a.payload)
        controller = MessageTurnController(parentPayload: a.payload, parent: a.env, mySeat: seat)
        ambiguous = nil
    }

    private func startGenesis() async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        controller = MessageTurnController(genesisSeed: seed, players: 2, gameId: gameId,
                                           myNickname: MessageGameStore.shared.nickname)
    }

    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
        MessageGameStore.shared.put(MessageGameRecord(
            gameId: env.gameId, mySeat: seat, nPlayers: env.nPlayers, round: env.round,
            turn: env.turn, phase: env.phase, finished: env.phase == 3, names: names,
            payloadBase32: Base32.encode(payload), updatedAt: Date().timeIntervalSince1970))
    }
}

/// §6.3 tertiary identity: N≥3, cache lost, not the last actor — ask the human.
private struct SeatPicker: View {
    let joins: [MessageJoin]
    let onPick: (Int) -> Void

    var body: some View {
        VStack(spacing: 12) {
            Text(FStrings.t("ios.msg.pickseat")).font(.headline)
            ForEach(joins, id: \.seat) { j in
                Button(j.name) { onPick(j.seat) }.buttonStyle(.bordered)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

private struct DamagedView: View {
    var body: some View {
        VStack(spacing: 8) {
            Text("Foolish").font(.headline)
            Text(FStrings.t("ios.msg.damaged")).font(.footnote)
                .multilineTextAlignment(.center).padding(.horizontal)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
