// MessageTableView — the INTERACTIVE expanded bubble (design §10). Same tap
// grammar as the app's TableView (tap a card to attack, select-then-tap-a-battle
// to cover, the wooden bar for pass/pickup/done) but driven by a
// MessageTurnController: a turn here STAGES a chain rather than committing to a
// live game, and the human presses Send (§11.4), never the code.
//
// Every enable state is the kernel's legal menu (`controller.legal`), never a
// hand-rolled "is it my turn" (§17.16). When my seat has no legal move I am a
// spectator on someone else's staged bubble — read-only, with a hint who is up.

import SwiftUI

public struct MessageTableView: View {
    @ObservedObject private var controller: MessageTurnController
    /// Seal the staged chain and hand it to the extension to compose + insert.
    /// The view never touches MSMessage; it only produces the payload.
    private let onSend: (Data) async -> Void

    @State private var selection: Set<String> = []
    @State private var toast: String?
    @State private var sending = false

    public init(controller: MessageTurnController, onSend: @escaping (Data) async -> Void) {
        self.controller = controller
        self.onSend = onSend
    }

    public var body: some View {
        VStack(spacing: 12) {
            if let view = controller.view {
                seats(view)
                center(view)
                Spacer(minLength: 0)
                statusLine(view)
                if controller.iCanAct {
                    actionBar(view)
                    hand(view)
                }
                sendBar
            } else {
                ProgressView()
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .fToast($toast, accent: true)
        .onChange(of: controller.rejectTick) { _ in
            Haptics.fire(.reject); toast = FStrings.t("ios.reject")
        }
        .task { if !controller.ready { await controller.begin() } }
    }

    // MARK: zones

    private func name(_ seat: Int) -> String { controller.names[seat] ?? "Seat \(seat + 1)" }

    private var attackersActive: Bool {
        guard let v = controller.view else { return false }
        return v.battles.contains { $0.defense == nil } || v.battles.isEmpty
    }

    private func seats(_ view: GameView) -> some View {
        HStack(spacing: 10) {
            ForEach(view.players) { p in
                FSeatBadge(name: name(p.seat),
                           handCount: p.handCount,
                           isDefender: p.seat == view.defender,
                           isAttacker: p.seat != view.defender && !p.isOut && attackersActive,
                           saidGood: view.hasSaidGood(p.seat),
                           isOut: p.isOut)
            }
        }
    }

    private func center(_ view: GameView) -> some View {
        HStack(alignment: .center, spacing: 16) {
            FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                      hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
            if view.battles.isEmpty {
                Text(FStrings.t("ios.nobattle")).font(.caption).foregroundStyle(.secondary)
            } else {
                FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit,
                            coverable: coverableBattles(view),
                            onTapBattle: { idx in tapBattle(idx, view) })
            }
            discard(view)
        }
    }

    private func discard(_ view: GameView) -> some View {
        VStack(spacing: 3) {
            Image(systemName: "rectangle.stack.fill").font(.title3).foregroundStyle(.secondary)
            Text("\(view.discardCount)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func statusLine(_ view: GameView) -> some View {
        if controller.isOver {
            Text(view.gameOver >= 0 ? "\(name(view.gameOver)) is the fool 🃏" : "game over")
                .font(.subheadline.weight(.semibold))
        } else if !controller.iCanAct {
            // No legal move for me on this staged state — I'm watching (§5.1).
            Text(waitingLine(view)).font(.footnote).foregroundStyle(.secondary)
        } else if controller.canSend {
            Text(FStrings.t("ios.msg.staged")).font(.footnote.weight(.medium))
        } else {
            Text(FStrings.t("ios.msg.yourmove")).font(.footnote).foregroundStyle(.secondary)
        }
    }

    private func waitingLine(_ view: GameView) -> String {
        // Name the defender if the table is uncovered, else "the others".
        let uncovered = view.battles.contains { $0.defense == nil }
        if uncovered, view.defender >= 0, view.defender != controller.mySeat {
            return FStrings.t("ios.msg.waitingfor", ["name": name(view.defender)])
        }
        return FStrings.t("ios.msg.waiting")
    }

    private func actionBar(_ view: GameView) -> some View {
        FActionBar(
            canPickup: has(.pickup),
            canDone: has(.good),
            canTransfer: transferMove() != nil,
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onTransfer: { if let m = transferMove() { play(m) } }
        )
    }

    private func hand(_ view: GameView) -> some View {
        FHandFan(cards: view.me?.hand ?? [], trumpSuit: view.trumpSuit,
                 selection: $selection, onTap: { tapCard($0, view) })
            .padding(.horizontal, FSpace.s)
    }

    @ViewBuilder
    private var sendBar: some View {
        if controller.canSend {
            HStack(spacing: FSpace.m) {
                FButton(FStrings.t("ios.msg.undo"), kind: .wood) { Task { await controller.undo() } }
                FButton(sending ? FStrings.t("ios.msg.sending") : FStrings.t("ios.msg.send"),
                        kind: .wood) {
                    guard !sending else { return }
                    sending = true
                    Task {
                        if let payload = try? await controller.stagedPayload() { await onSend(payload) }
                        sending = false
                    }
                }
            }
            .padding(.top, 2)
        }
    }

    // MARK: interaction (mirrors TableView — every branch reads the kernel menu)

    private func play(_ move: Move) { selection.removeAll(); Task { await controller.apply(move) } }

    private func has(_ type: MoveType) -> Bool { controller.legal.contains { $0.type == type } }

    private func tapCard(_ card: Card, _ view: GameView) {
        if let atk = controller.legal.first(where: { $0.type == .attack && $0.cards == [card] }) {
            play(atk); return
        }
        let canCover = controller.legal.contains { $0.type == .cover && $0.cards == [card] }
        let canTransfer = controller.legal.contains { $0.type == .pass && $0.cards.contains(card) }
        if canCover || canTransfer {
            selection = [card.identity]
        } else {
            Haptics.fire(.reject); toast = FStrings.t("ios.reject")
        }
    }

    private func tapBattle(_ index: Int, _ view: GameView) {
        guard index < view.battles.count,
              let selId = selection.first,
              let card = view.me?.hand?.first(where: { $0.identity == selId }) else { return }
        let attack = view.battles[index].attack
        if let cover = controller.legal.first(where: {
            $0.type == .cover && $0.cards == [card] && ($0.attackCards ?? []) == [attack]
        }) {
            play(cover)
        } else {
            Haptics.fire(.reject)
        }
    }

    private func transferMove() -> Move? {
        guard let selId = selection.first else { return nil }
        return controller.legal.first { $0.type == .pass && $0.cards.contains { $0.identity == selId } }
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        guard let selId = selection.first,
              let card = view.me?.hand?.first(where: { $0.identity == selId }) else { return [] }
        var out: Set<Int> = []
        for (i, b) in view.battles.enumerated() where controller.legal.contains(where: {
            $0.type == .cover && $0.cards == [card] && ($0.attackCards ?? []) == [b.attack]
        }) { out.insert(i) }
        return out
    }
}
