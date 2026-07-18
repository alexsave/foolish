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
                    // Once a move is staged, drop the action bar (Done/Take/Transfer):
                    // the move is made. The only follow-ups are adding another card
                    // (hand) or Undo — and the extension has already dropped the user
                    // at Messages' Send. Keeping Done here was a third redundant
                    // "send" affordance next to the staged text and the send arrow.
                    if !controller.canSend { actionBar(view) }
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
        .task {
            if !controller.ready { await controller.begin() }
            // Genesis where I can't act (I dealt but I'm not the first attacker):
            // stage the deal immediately so I can send it on. When I CAN act,
            // canStage is false until I play, so this is a no-op then.
            await stageNow()
            #if DEBUG
            // FoolishHarness screenshotting only: auto-play the first legal move so
            // the auto-stage flow (move -> staged bubble) is visible without a tap.
            if ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE"] != nil,
               let m = controller.legal.first(where: { $0.type != .wait }) {
                play(m)
            }
            #endif
        }
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
                Text(FStrings.t("ios.nobattle")).font(.caption).foregroundStyle(FColor.textDim)
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
            Image(systemName: "rectangle.stack.fill").font(.title3).foregroundStyle(FColor.textDim)
            Text("\(view.discardCount)").font(.caption.monospacedDigit()).foregroundStyle(FColor.textDim)
        }
    }

    @ViewBuilder
    private func statusLine(_ view: GameView) -> some View {
        if controller.isOver {
            statusChip(view.gameOver >= 0
                ? FStrings.t("ios.msg.isfool", ["name": name(view.gameOver)])
                : FStrings.t("game_over"), strong: true)
        } else if controller.canStage {
            // Something is sendable (a staged move, or a genesis deal to hand on).
            // Say nothing: the extension collapses to Messages' Send on stage, so a
            // "Move staged - hit Send" line is redundant with the send arrow itself.
            EmptyView()
        } else if !controller.iCanAct {
            // No legal move for me on this staged state — I'm watching (§5.1).
            statusChip(waitingLine(view))
        } else {
            statusChip(FStrings.t("ios.msg.yourmove"))
        }
    }

    /// The status text on a dark pill so it stays readable over the busy wool
    /// table (B4 bug: "Waiting for the others" was near-invisible).
    private func statusChip(_ text: String, strong: Bool = false) -> some View {
        Text(text)
            .font(strong ? .subheadline.weight(.semibold) : .footnote.weight(.medium))
            .foregroundStyle(FColor.textPrimary)
            .padding(.horizontal, 12).padding(.vertical, 5)
            .background(Capsule().fill(.black.opacity(0.45)))
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
        let cards = selectedCards(view)
        let defending = view.defender == controller.mySeat
        return FActionBar(
            canAttack: !defending && CardPlay.canAttack(cards, legal: controller.legal),
            canCover: defending && CardPlay.canCover(cards, battles: view.battles, legal: controller.legal),
            canPass: defending && CardPlay.canPass(cards, legal: controller.legal),
            canPickup: has(.pickup),
            canDone: has(.good),
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) }
        )
    }

    private func hand(_ view: GameView) -> some View {
        FHandFan(cards: view.me?.hand ?? [], trumpSuit: view.trumpSuit,
                 selection: $selection, onTap: { toggle($0) })
            .padding(.horizontal, FSpace.s)
    }

    // A staged move auto-composes the bubble (see `stageNow`), so the only send
    // control left is Undo — the human's next tap is the Messages send arrow.
    @ViewBuilder
    private var sendBar: some View {
        if controller.canSend {
            FButton(FStrings.t("ios.msg.undo"), kind: .wood) {
                Task { await controller.undo(); await stageNow() }
            }
            .padding(.top, 2)
        }
    }

    // MARK: interaction (mirrors TableView — every branch reads the kernel menu)

    private func play(_ move: Move) {
        selection.removeAll()
        Task { await controller.apply(move); await stageNow() }
    }

    /// Compose + stage the bubble the instant a move is applied (§11.4 flow, B4
    /// feedback: "too many buttons before you can send"). Playing an attack /
    /// cover / pickup / pass / good drops you straight to the staged message, so
    /// the only remaining action is the send arrow. Re-staging replaces the input
    /// bubble, so throwing in more cards just updates it. No-op until something is
    /// staged (a 0-action genesis body is unsealable).
    private func stageNow() async {
        guard controller.canStage else { return }
        if let payload = try? await controller.stagedPayload() { await onSend(payload) }
    }

    private func has(_ type: MoveType) -> Bool { controller.legal.contains { $0.type == type } }

    // Dumb selection; CardPlay resolves (selection, target) into one legal move,
    // exactly like the app's TableView (both read the kernel menu, never a rule).

    private func toggle(_ card: Card) {
        if selection.contains(card.identity) { selection.remove(card.identity) }
        else { selection.insert(card.identity) }
    }

    private func selectedCards(_ view: GameView) -> [Card] {
        (view.me?.hand ?? []).filter { selection.contains($0.identity) }
    }

    /// Tap an uncovered attack while cards are selected → cover it (two-tap cover).
    private func tapBattle(_ index: Int, _ view: GameView) {
        playAt(.battle(index), selectedCards(view), view)
    }

    private func playAt(_ target: PlayTarget, _ cards: [Card], _ view: GameView) {
        guard let move = CardPlay.resolve(cards: cards, target: target,
                                          isDefender: view.defender == controller.mySeat,
                                          battles: view.battles, legal: controller.legal) else {
            Haptics.fire(.reject); toast = FStrings.t("ios.reject"); return
        }
        play(move)
    }

    /// Cover button: cover the first uncovered attack the selection can beat.
    private func playCover(_ cards: [Card], _ view: GameView) {
        guard let i = CardPlay.coverableBattles(cards: cards, battles: view.battles,
                                                legal: controller.legal).sorted().first else {
            Haptics.fire(.reject); return
        }
        playAt(.battle(i), cards, view)
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        let out = CardPlay.coverableBattles(cards: selectedCards(view), battles: view.battles, legal: controller.legal)
        return out
    }
}
