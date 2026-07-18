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
    // Drag-to-play state (frames published by FBattleGrid/FHandFan in `boardSpace`).
    @State private var battleFrames: [Int: CGRect] = [:]
    @State private var handFrame: CGRect = .zero
    @State private var dragCard: Card?
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    /// Shared card-flight namespace: a card keeps its identity moving hand→table.
    @Namespace private var cardNS
    // Overlay flights to the discard pile (bout end), where matchedGeometry has no
    // target view to match against.
    @StateObject private var animator = BoardAnimator()
    @State private var lastView: GameView?
    @State private var discardFrame: CGRect = .zero
    /// The most recent NON-empty battle rects, so a bout-end flight still has the
    /// source positions after the table cleared (preference vs onChange can race).
    @State private var lastBattleFrames: [Int: CGRect] = [:]
    @State private var deckFrame: CGRect = .zero
    @State private var handCardFrames: [String: CGRect] = [:]
    /// Cards drawn this bout end, awaiting their new hand-slot rects to fly in.
    @State private var pendingDraws: [Card] = []

    public init(controller: MessageTurnController, onSend: @escaping (Data) async -> Void) {
        self.controller = controller
        self.onSend = onSend
    }

    public var body: some View {
        VStack(spacing: 8) {
            if let view = controller.view {
                // Top row (web corners): deck + flipped trump on the LEFT, the
                // opponents between, the discard pile on the RIGHT. Keeping deck and
                // discard out of the centre leaves the battles the full width.
                HStack(alignment: .top, spacing: 8) {
                    FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                              hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                    Spacer(minLength: 0)
                    opponents(view)
                    Spacer(minLength: 0)
                    FDiscardPile(count: view.discardCount)
                }
                Spacer(minLength: 0)
                battlesArea(view)          // full-width wrapping attack/cover pairs
                Spacer(minLength: 0)
                statusLine(view)
                // The vertical button column: the play buttons when I can act, or
                // Undo once a move is staged (FActionBar shows whichever the flags
                // enable — nothing while waiting).
                actionBar(view)
                // Always show my own hand — even while "waiting for the others"
                // (web parity: the hand is visible whether or not I have a legal
                // move; legality is expressed by which buttons appear, not by
                // hiding the cards).
                hand(view)
            } else {
                ProgressView()
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: controller.view)
        .overlay { FlyingCardsLayer(animator: animator) }
        .coordinateSpace(name: boardSpace)
        .onPreferenceChange(BattleFramesKey.self) { fr in
            battleFrames = fr
            if !fr.isEmpty { lastBattleFrames = fr }
        }
        .onPreferenceChange(HandFrameKey.self) { handFrame = $0 }
        .onPreferenceChange(DiscardFrameKey.self) { discardFrame = $0 }
        .onPreferenceChange(DeckFrameKey.self) { deckFrame = $0 }
        .onPreferenceChange(HandCardFramesKey.self) { handCardFrames = $0; tryDrawFlight() }
        .onChange(of: controller.view) { flyBoutEndToDiscard(to: $0) }
        .onChange(of: animator.isAnimating) { _ in tryDrawFlight() }
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

    /// Opponent badges only — self is the hand at the bottom (web PlayerRing seats
    /// everyone but self, who owns the bottom edge).
    private func opponents(_ view: GameView) -> some View {
        HStack(spacing: 10) {
            ForEach(view.players.filter { $0.seat != controller.mySeat }) { p in
                FSeatBadge(name: name(p.seat),
                           handCount: p.handCount,
                           isDefender: p.seat == view.defender,
                           isAttacker: p.seat != view.defender && !p.isOut && attackersActive,
                           saidGood: view.hasSaidGood(p.seat),
                           isOut: p.isOut)
            }
        }
    }

    private func battlesArea(_ view: GameView) -> some View {
        Group {
            if view.battles.isEmpty {
                Text(FStrings.t("ios.nobattle")).font(.caption).foregroundStyle(FColor.textDim)
            } else {
                FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit,
                            coverable: highlightBattles(view),
                            onTapBattle: { idx in tapBattle(idx, view) },
                            namespace: cardNS, hidden: animator.hidden)
            }
        }
        .frame(maxWidth: .infinity)
    }

    /// The cards a play would use right now: the whole selection if the dragged (or
    /// tapped) card is part of it, else just that one card (web selected-or-single).
    private func playCards(for card: Card, _ view: GameView) -> [Card] {
        selection.contains(card.identity) ? selectedCards(view) : [card]
    }

    /// Battles to highlight — what the in-flight drag (or the current selection)
    /// could cover.
    private func highlightBattles(_ view: GameView) -> Set<Int> {
        let cards = dragCard.map { playCards(for: $0, view) } ?? selectedCards(view)
        return CardPlay.coverableBattles(cards: cards, battles: view.battles, legal: controller.legal)
    }

    /// When a bout closes (the table clears into the discard), fly the cards that
    /// were on the table to the discard pile — the web's cards_to_trash flight,
    /// as a small overlay (matchedGeometry has no discard-side view to match). Reads
    /// the battle rects still held from before the view cleared.
    private func flyBoutEndToDiscard(to newView: GameView?) {
        let prior = lastView
        lastView = newView
        guard !reduceMotion, let new = newView else { return }
        // First time the board appears with a delivered game: replay the last move
        // that produced this bubble (the web "open the message, watch what happened").
        if prior == nil { replayLastMoveOnOpen(new); return }
        guard let old = prior,
              !old.battles.isEmpty, new.battles.isEmpty,
              new.discardCount > old.discardCount, discardFrame != .zero else { return }
        var flights: [Flight] = []
        for (i, b) in old.battles.enumerated() {
            guard let rect = lastBattleFrames[i], rect != .zero else { continue }
            flights.append(Flight(id: "trash-\(b.attack.identity)", card: b.attack, from: rect, to: discardFrame))
            if let d = b.defense {
                flights.append(Flight(id: "trash-\(d.identity)", card: d,
                                      from: rect.offsetBy(dx: 8, dy: 6), to: discardFrame))
            }
        }
        // Draws: the cards this seat gained on the bout close fly deck→hand AFTER
        // the discard (web sequence: cards_to_trash then refill). Held until their
        // new hand slots render (tryDrawFlight fires when the rects + discard land).
        let oldHand = Set((old.me?.hand ?? []).map(\.identity))
        pendingDraws = (new.me?.hand ?? []).filter { !oldHand.contains($0.identity) }

        guard !flights.isEmpty else { return }
        Task { await animator.play([flights]) }
    }

    /// On opening a delivered bubble, replay the last card-placing move: fly the
    /// card(s) it put on the table into their battle slots, from above (the sender's
    /// side), so you SEE what the last move was — the web's open-a-message replay.
    /// Reads the last LOG_ATTACK/LOG_COVER from the packed replay stream (no JSON).
    private func replayLastMoveOnOpen(_ view: GameView) {
        guard !reduceMotion, !view.battles.isEmpty, !controller.isOver else { return }
        Task {
            // Let the battle slots render + publish their rects first.
            try? await Task.sleep(nanoseconds: 120_000_000)
            guard let replay = await MessageKernel.shared.residentReplay() else { return }
            let LOG_ATTACK = 1, LOG_COVER = 2   // game.h LOG_* (packed step types)
            guard let last = replay.logs.last(where: { $0.type == LOG_ATTACK || $0.type == LOG_COVER })
            else { return }
            var flights: [Flight] = []
            for card in last.pairs.map(\.primary) where !card.isHidden {
                guard let idx = view.battles.firstIndex(where: { $0.attack == card || $0.defense == card }),
                      let rect = battleFrames[idx] else { continue }
                let from = rect.offsetBy(dx: 0, dy: -220)   // fly down from the sender's side
                flights.append(Flight(id: "open-\(card.identity)", card: card, from: from, to: rect))
            }
            guard !flights.isEmpty else { return }
            await animator.play([flights])
        }
    }

    /// Fly this bout's drawn cards from the deck to their new hand slots — once the
    /// discard flight is done and the new slots have measured rects.
    private func tryDrawFlight() {
        guard !pendingDraws.isEmpty, !animator.isAnimating, deckFrame != .zero,
              pendingDraws.allSatisfy({ handCardFrames[$0.identity] != nil }) else { return }
        let flights = pendingDraws.compactMap { c -> Flight? in
            guard let to = handCardFrames[c.identity] else { return nil }
            return Flight(id: "draw-\(c.identity)", card: c, from: deckFrame, to: to)
        }
        pendingDraws = []
        Task { await animator.play([flights]) }
    }

    private func onDragChanged(_ card: Card) { dragCard = card }

    private func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        dragCard = nil
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handFrame)
        if target == .hand { return }   // dropped back in the fan — cancel
        playAt(target, playCards(for: card, view), view)
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
        // Play buttons only while I can act and have NOT staged; once staged, the
        // only control is Undo (the extension has dropped the user at Messages' Send).
        let acting = controller.iCanAct && !controller.canSend
        return FActionBar(
            canAttack: acting && !defending && CardPlay.canAttack(cards, legal: controller.legal),
            canCover: acting && defending && CardPlay.canCover(cards, battles: view.battles, legal: controller.legal),
            canPass: acting && defending && CardPlay.canPass(cards, legal: controller.legal),
            canPickup: acting && has(.pickup),
            canDone: acting && CardPlay.canSayGood(battles: view.battles, legal: controller.legal),
            canUndo: controller.canSend,
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onUndo: { Task { await controller.undo(); await stageNow() } }
        )
    }

    private func hand(_ view: GameView) -> some View {
        FHandFan(cards: view.me?.hand ?? [], trumpSuit: view.trumpSuit,
                 selection: $selection, onTap: { toggle($0) },
                 onDragChanged: { card, _ in onDragChanged(card) },
                 onDragEnded: { card, point in onDragEnded(card, at: point, view) },
                 namespace: cardNS, hidden: animator.hidden)
            .padding(.horizontal, FSpace.s)
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
