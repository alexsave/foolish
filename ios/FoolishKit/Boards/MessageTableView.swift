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
import Foundation   // sin/cos for the ring placement

public struct MessageTableView: View {
    @ObservedObject private var controller: MessageTurnController
    /// Seal the staged chain and hand it to the extension to compose + insert.
    /// The view never touches MSMessage; it only produces the payload.
    private let onSend: (Data) async -> Void
    /// Start a fresh game in this thread. Offered on the board only once the game
    /// is over (the fool is decided) — any player, out or not, can deal the next
    /// one. Routes through the host's New game (§5.2), same as the chrome button.
    private let onNewGame: () -> Void
    /// Retract a bubble already staged with the host (§10 undo). Undo alone can't
    /// do this: rebuilding the base + replaying `pending` minus the last action is
    /// a LOCAL replay, but the host (harness `staged`, or the real extension's
    /// already-inserted input-field bubble) is a separate piece of state that only
    /// the host can clear. Called when an undo empties `pending` entirely.
    private let onUnstage: () -> Void

    @State private var selection: Set<String> = []
    @State private var toast: String?
    // Drag-to-play state (frames published by FBattleGrid/FHandFan in `boardSpace`).
    @State private var battleFrames: [Int: CGRect] = [:]
    @State private var handFrame: CGRect = .zero
    @State private var dragCard: Card?
    /// notes 33/34: the drag's live point in `boardSpace`, kept (FHandFan
    /// already delivers it on every `onDragChanged`, previously discarded)
    /// so the verb hint and the pass ghost-slot preview can resolve the SAME
    /// drop target `onDragEnded` will use. nil whenever no drag is active.
    @State private var dragPoint: CGPoint?
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
    @State private var seatFrames: [Int: CGRect] = [:]
    // Displayed counts LAG the game state during a bout-end sequence: a badge/deck/
    // discard count holds its old value until that card's flight lands, then bumps
    // (so a hand count never jumps before the deck→player draw animation plays).
    @State private var seatCountOverride: [Int: Int] = [:]
    @State private var deckCountOverride: Int?
    @State private var discardCountOverride: Int?
    /// note 39: the board keeps its stage until whatever's animating (a
    /// bout-end sequence, or an open-delta replay) visibly finishes — the
    /// end screen only swaps in once this flips. Starts false; `.task` resets
    /// it defensively in case this view ever survives a controller swap
    /// (structurally it doesn't today — GameSurface always nils `controller`
    /// before assigning a new one — but nothing here should rely on that).
    @State private var showResults = false
    /// note 17: a cover that's about to empty the defender's hand ends the
    /// bout in the SAME kernel apply as the cover itself, so
    /// `flyBoutEndToDiscard` never sees an intermediate "covered" table to
    /// animate from — the card would just vanish straight to the discard
    /// pile. Stashed by `playAt` right before `controller.apply`, consumed
    /// (and always cleared) by the very next `flyBoutEndToDiscard` call.
    private struct PendingCover {
        let cards: [Card]
        let battleRect: CGRect
        let fromRects: [String: CGRect]   // card.identity -> hand rect AT PLAY TIME
    }
    @State private var pendingCover: PendingCover?

    public init(controller: MessageTurnController, onSend: @escaping (Data) async -> Void,
                onNewGame: @escaping () -> Void = {}, onUnstage: @escaping () -> Void = {}) {
        self.controller = controller
        self.onSend = onSend
        self.onNewGame = onNewGame
        self.onUnstage = onUnstage
    }

    public var body: some View {
        VStack(spacing: 8) {
            if let view = controller.view {
                // Game over: the board gives way to the ranked results screen (web
                // WinScreen parity) - finish order first-out down to the fool, with
                // New game there. Gated on `showResults` too (note 39): the board
                // stays the stage until the last flight (a bout-end sequence, or an
                // open-delta replay of someone else's final move) has visibly
                // landed, so the end screen never just cuts in mid-animation.
                if controller.isOver && showResults {
                    FGameOverList(rows: finishRows(view), onNewGame: onNewGame)
                } else {
                    // The card-motion spring is scoped to the LIVE board only (note
                    // 18/40): it used to sit on this whole VStack, so the swap TO
                    // this branch's FGameOverList animated implicitly too — the
                    // WoodFill plank grew in under the spring, reading as the
                    // background "zooming" (worse with more rows; invisible in 2p,
                    // hence note 40). Attaching it here instead of on the root means
                    // in-board changes (deck count, hand, seat badges) still animate,
                    // but the board→results swap does not.
                    boardContent(view)
                        .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: controller.view)
                }
            } else {
                ProgressView()
            }
        }
        .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)   // top margin so the ring isn't clipped in the compact drawer
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay { FlyingCardsLayer(animator: animator) }
        .coordinateSpace(name: boardSpace)
        .onPreferenceChange(BattleFramesKey.self) { fr in
            battleFrames = fr
            if !fr.isEmpty { lastBattleFrames = fr }
        }
        .onPreferenceChange(HandFrameKey.self) { handFrame = $0 }
        .onPreferenceChange(DiscardFrameKey.self) { discardFrame = $0 }
        .onPreferenceChange(DeckFrameKey.self) { deckFrame = $0 }
        .onPreferenceChange(SeatFramesKey.self) { seatFrames = $0 }
        .onPreferenceChange(HandCardFramesKey.self) { handCardFrames = $0 }
        .onChange(of: controller.view) { flyBoutEndToDiscard(to: $0) }
        .fToast($toast, accent: true)
        .onChange(of: controller.rejectTick) { _ in
            Haptics.fire(.reject); toast = FStrings.t("ios.reject")
        }
        .task {
            // note 39: defensive reset — see `showResults`'s doc.
            showResults = false
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
                // Let the incoming replay (the OTHER player's last move flying
                // deck/seat→table on open) finish and rest so it's watchable,
                // THEN auto-play our move. Dev pacing only.
                try? await Task.sleep(nanoseconds: 2_400_000_000)
                play(m)
            }
            #endif
        }
    }

    /// The live board, laid out like the web GameBoard: every piece is placed
    /// ABSOLUTELY against the board rect, so the centred pieces never shift when a
    /// corner changes (the bug where the deck pushed the opponent off-centre and
    /// clipped it). Deck pins top-left, discard top-right; opponents ring a 35%
    /// ellipse (the local player is the hand, so it is omitted); the battles sit
    /// dead-centre; my hand hugs the bottom. The first-attacker SWORD (not a "your
    /// move" label that ate layout) marks that I must open the bout.
    private func boardContent(_ view: GameView) -> some View {
        GeometryReader { geo in
            ZStack {
                // Battles — dead centre of the board (web: absolute, both axes).
                battlesArea(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                // Opponent ring — each seat placed by trig on a 35% ellipse. The
                // local player is visual-index 0 (bottom edge) and is drawn as the
                // hand, so it is skipped here.
                ForEach(view.players.filter { $0.seat != controller.mySeat }) { p in
                    opponentSeat(p, view)
                        .position(ringPoint(seat: p.seat, n: view.players.count, in: geo.size))
                }

                // Deck top-left, discard top-right — pinned to the corners and OUT
                // of the centred flow, so they never push the ring or battles.
                FDeckWell(deckCount: deckCountOverride ?? view.deckCount, flipped: view.flipped,
                          hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                    // FDeckWell now anchors its own content top-leading with a
                    // small symmetric inset (note 14), so no per-call-site
                    // compensation offset is needed here anymore.
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                FDiscardPile(count: discardCountOverride ?? view.discardCount)
                    .offset(y: -16)   // snug into the top-right corner
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                // Self role indicator: the local seat never got a role mark before
                // (note 3) — only opponents (FSeatBadge) did. Same spot the old
                // first-attacker-only sword used: just above my hand.
                selfRoleIndicator(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .padding(.bottom, 86)

                // Action buttons float bottom-right, above the hand (web absolute
                // bottom:90/right:20). They only appear when a flag enables them.
                actionBar(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(.trailing, 4).padding(.bottom, 84)

                // My hand hugs the bottom (web: bottom max(10, safe-area)); the
                // outer .padding(12) is the safe-area inset that keeps it unclipped.
                hand(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            }
        }
    }

    /// The finish order for the end screen: rank 1 = first player out (best),
    /// counting up to the fool last. `eliminationOrder` is first-out first and
    /// holds everyone except the fool; the fool is `view.gameOver` (the one seat
    /// still holding cards), given the last place. Mirrors web WinScreen.
    private func finishRows(_ view: GameView) -> [FinishRow] {
        let total = view.players.count
        var rows: [FinishRow] = []
        for (i, seat) in view.eliminationOrder.enumerated() {
            rows.append(FinishRow(place: i + 1, total: total, name: name(seat),
                                  isYou: seat == controller.mySeat))
        }
        if view.gameOver >= 0 {
            rows.append(FinishRow(place: total, total: total, name: name(view.gameOver),
                                  isYou: view.gameOver == controller.mySeat))
        }
        return rows
    }

    // MARK: zones

    private func name(_ seat: Int) -> String { controller.names[seat] ?? "Seat \(seat + 1)" }

    /// One opponent seat badge, publishing its frame in `boardSpace` so bout-end
    /// flights can target it. Placed on the ring by `ringPoint`.
    private func opponentSeat(_ p: PlayerView, _ view: GameView) -> some View {
        // Sword consistency: an attacker keeps the sword until THEY say good, even
        // once every attack on the table is covered — not "any uncovered battle
        // exists" (which erased every sword the instant the table was fully covered).
        FSeatBadge(name: name(p.seat),
                   handCount: seatCountOverride[p.seat] ?? p.handCount,
                   isDefender: p.seat == view.defender,
                   isAttacker: p.seat != view.defender && !p.isOut && !view.hasSaidGood(p.seat),
                   saidGood: view.hasSaidGood(p.seat),
                   isOut: p.isOut)
            .background(GeometryReader { g in
                Color.clear.preference(key: SeatFramesKey.self,
                                       value: [p.seat: g.frame(in: .named(boardSpace))])
            })
    }

    /// A seat's centre on the web's 35% ellipse (PlayerRing): the local player is
    /// visual-index 0 → bottom centre (drawn as the hand); opponents fan clockwise.
    /// Percentages resolve against width vs height, which reads as an oval on a
    /// non-square board - exactly the web's trick.
    private func ringPoint(seat: Int, n: Int, in size: CGSize) -> CGPoint {
        let visual = (seat - controller.mySeat + n) % n
        let rad = 2 * Double.pi * Double(visual) / Double(max(n, 1))
        // A compressed board (the compact drawer) pushes the ring a little higher
        // so it and the hand don't crowd the middle - but only a little, or the
        // top badge clips against the drawer's rounded top edge.
        let ry = size.height < 340 ? 0.38 : 0.35
        let x = (-sin(rad) * 0.35 + 0.5) * size.width
        let y = ( cos(rad) * ry + 0.5) * size.height
        return CGPoint(x: x, y: y)
    }

    /// My own role mark — shield/check/sword — mirroring FSeatBadge's roleRow for
    /// opponents (note 3: the local player never saw their own role before, only
    /// the special-cased first-attacker sword). Nothing shows once I'm out (the
    /// game-over screen replaces the whole board, so "game over" is already
    /// handled by the caller never reaching here then).
    private func selfRoleIndicator(_ view: GameView) -> some View {
        let mySeat = controller.mySeat
        let isOut = view.me?.isOut ?? false
        let isDefender = view.defender == mySeat
        let saidGood = view.hasSaidGood(mySeat)
        let isAttacker = !isDefender && !isOut && !saidGood
        return Group {
            if !isOut {
                HStack(spacing: FSpace.xs) {
                    if saidGood { FCheck(size: 19) }
                    if isDefender { FShield(size: 22) }
                    else if isAttacker { FSword(size: 19) }
                }
            }
        }
    }

    private func battlesArea(_ view: GameView) -> some View {
        Group {
            if view.battles.isEmpty {
                // Empty table: render nothing (web parity). A "no battle" label just
                // tells the player what they can already see (owner's call).
                Color.clear
            } else {
                // note 34: a pass preview shows the ghost slot instead of a cover
                // highlight — the card that would get passed isn't being covered,
                // so nothing on the table should look like a drop target for it.
                let passPreview = isPassPreview(view)
                FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit,
                            coverable: passPreview ? [] : highlightBattles(view),
                            onTapBattle: { idx in tapBattle(idx, view) },
                            namespace: cardNS, hidden: animator.hidden,
                            showGhostSlot: passPreview)
            }
        }
        // note 33: the verb hint floats just above the battle grid's own
        // (self-sized) top edge — attached here, BEFORE the maxWidth/maxHeight
        // expansion below, so it tracks the grid's actual content regardless of
        // row count instead of a fixed board-relative offset guessed once and
        // never re-checked against a real device.
        .overlay(alignment: .top) { dragHint(view).offset(y: -30) }
        .frame(maxWidth: .infinity)   // boardContent's call site adds maxHeight
    }

    /// The move a release right now would resolve to, if any — the SAME
    /// `BoardDrop.target` + `CardPlay.resolve` math `onDragEnded` uses, shared
    /// by the verb hint (note 33) and the pass ghost-slot preview (note 34) so
    /// neither can disagree with what actually happens on release.
    private func dragPreview(_ view: GameView) -> (target: PlayTarget, move: Move)? {
        guard let card = dragCard, let point = dragPoint else { return nil }
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handFrame)
        guard target != .hand,
              let move = CardPlay.resolve(cards: playCards(for: card, view), target: target,
                                          isDefender: view.defender == controller.mySeat,
                                          battles: view.battles, legal: controller.legal)
        else { return nil }
        return (target, move)
    }

    /// note 34: is the live drag currently previewing a PASS onto open table
    /// space? Gates both the ghost slot and the suppression of the ordinary
    /// per-battle cover highlight while it's showing.
    private func isPassPreview(_ view: GameView) -> Bool {
        guard let preview = dragPreview(view) else { return false }
        return preview.target == .table && preview.move.type == .pass
    }

    /// note 33: what a release would do, localized — "Attack" / "Cover" /
    /// "Pass". Nothing over the hand (that's a reorder, not a play) or for a
    /// drop `CardPlay` can't resolve into a legal move.
    private func dragHintText(_ view: GameView) -> String? {
        guard let move = dragPreview(view)?.move else { return nil }
        switch move.type {
        case .attack: return FStrings.t("attack")
        case .cover: return FStrings.t("cover")
        case .pass: return FStrings.t("pass")
        default: return nil
        }
    }

    /// note 33: a small unobtrusive pill naming what release would do — web
    /// DragShadow parity, but anchored to a fixed spot above the battles
    /// rather than tracking the fingertip (simplest robust placement: no
    /// per-frame layout math, and it can never end up under the dragging
    /// hand or over the action bar).
    @ViewBuilder
    private func dragHint(_ view: GameView) -> some View {
        if let text = dragHintText(view) {
            Text(text)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(FColor.card)
                .padding(.horizontal, FSpace.m)
                .padding(.vertical, FSpace.xs)
                .background(FColor.ink.opacity(0.85))
                .clipShape(RoundedRectangle(cornerRadius: FRadius.chip))
                .shadow(color: .black.opacity(0.35), radius: 6, y: 2)
                .transition(.opacity)
                .allowsHitTesting(false)
        }
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

    /// When a bout closes (the table clears), run the web's ORDERED bout-end
    /// sequence: first the table clears (cards → discard when beaten, or → the taker
    /// on a pickup), THEN each drawing player refills from the deck as its OWN step,
    /// in the kernel's exact order (the LOG_DRAW stream: defender-if-empty, then from
    /// the first attacker around). Every step waits for its frames to render, plays,
    /// and is awaited - so nothing "just jumps" and draws never overlap. (This
    /// mirrors evwire's cards_to_trash → one refill per player → defender_move.)
    private func flyBoutEndToDiscard(to newView: GameView?) {
        let prior = lastView
        lastView = newView
        // note 17: consumed and cleared on EVERY call, whether or not this
        // turns out to be the matching bout-end transition — `playAt` sets it
        // right before the apply whose resulting view change is always the
        // very next one this function sees.
        let cover = pendingCover
        pendingCover = nil
        guard let new = newView else { return }
        if reduceMotion {
            // note 39b: nothing will ever animate, so settle immediately.
            if new.isOver { showResults = true }
            return
        }
        // note 10: undo can legally take battles → empty (undoing the move
        // that opened a bout) — that must never be misread as a bout end and
        // replay the PREVIOUS bout's draws.
        if controller.lastChangeWasUndo { return }
        // First time the board appears with a delivered game: replay the last move.
        if prior == nil { replayLastMoveOnOpen(new); return }
        guard let old = prior, !old.battles.isEmpty, new.battles.isEmpty else {
            // No bout-end sequence here (a normal placed card animates via
            // matchedGeometry alone, or the move ended the game WITHOUT
            // clearing the table — an attacker/passer running out of cards
            // with none left in the deck, game.c's early PLAYER_OUT paths).
            // Still settle to results once that ambient spring finishes.
            if new.isOver { settleResults() }
            return
        }

        let beaten = new.discardCount > old.discardCount
        let oldBattles = old.battles
        let boutFrames = lastBattleFrames
        let oldHandIds = Set((old.me?.hand ?? []).map(\.identity))
        let myNewCards = (new.me?.hand ?? []).filter { !oldHandIds.contains($0.identity) }
        // The pickup taker (nil when beaten) is the seat that WAS defending — the
        // kernel only lets the defender pick up (`handle_pickup` rejects any other
        // seat, c/src/game.c) — read off the PRE-move view, because handle_pickup
        // reassigns g->defender before returning, so `new.defender` is already the
        // NEXT bout's defender.
        //
        // This used to be "the first seat whose hand grew", which was wrong twice
        // over: handle_pickup calls refill_player_hands in the SAME apply, so
        // attackers' hands grow too, and `players` is seat-ascending, so the search
        // returned the LOWEST-seated refilled attacker whenever that seat sat below
        // the defender. That flew the whole table to the wrong badge (the reported
        // "pickup animates to the player on the right"), or into MY hand if the
        // misattributed seat happened to be mine, and then suppressed that seat's
        // real draw further down.
        let takerSeat: Int? = beaten ? nil : old.defender
        // note 17: does the stashed pending cover belong to THIS transition?
        // Its battle rect must have been part of the table we just cleared.
        var matchedCover: PendingCover?
        if beaten, let pc = cover, boutFrames.values.contains(pc.battleRect) { matchedCover = pc }

        // note 36: myNewCards already covers BOTH cases a viewer can land in
        // my hand here — a draw (the ordinary refill loop below), or a
        // pickup where I'm the taker (pickupToHandFlights below) — because a
        // taker never also draws in the same bout (game.c's refill loop
        // excludes them). Pre-hide synchronously, before the Task starts, so
        // neither path renders the card first and flies it in second.
        if !myNewCards.isEmpty { animator.preHide(Set(myNewCards.map(\.identity))) }

        // Freeze every displayed count to its PRE-refill value (set now, before the
        // async steps) so a badge/deck/discard never jumps ahead of its flight.
        for p in old.players where p.seat != controller.mySeat { seatCountOverride[p.seat] = p.handCount }
        deckCountOverride = old.deckCount
        discardCountOverride = old.discardCount

        Task {
            BoardAnimator.sequenceDepth += 1
            defer {
                BoardAnimator.sequenceDepth -= 1
                seatCountOverride = [:]; deckCountOverride = nil; discardCountOverride = nil
                animator.clearPreHidden()
            }

            // STEP 1 — the table clears; its count bumps only AFTER the flight lands.
            if beaten {
                if let pc = matchedCover {
                    // note 17: the cover lands on its battle FIRST — the kernel
                    // jumped straight from "uncovered" to "table cleared" in one
                    // apply, so there's no intermediate rendered state to fly
                    // this card from otherwise.
                    await playStep { self.pendingCoverLandingFlights(pc) }
                }
                await playStep { self.discardFlights(oldBattles, boutFrames, extraCover: matchedCover) }
                discardCountOverride = new.discardCount
            } else if let taker = takerSeat {
                if taker == controller.mySeat {
                    await playStep { self.pickupToHandFlights(oldBattles, boutFrames) }
                } else {
                    await playStep { self.pickupToBadgeFlights(oldBattles, boutFrames, seat: taker) }
                    seatCountOverride[taker] = new.players.first { $0.seat == taker }?.handCount
                }
            }

            // STEPS 2..N — each drawing player refills, in kernel order, one at a
            // time. The player's count (and the deck) bump only after THAT draw lands.
            if let replay = await MessageKernel.shared.residentReplay() {
                for ev in Self.lastBoutDraws(replay) where ev.seat != (takerSeat ?? -1) {
                    if ev.seat == controller.mySeat {
                        await playStep { self.myDrawFlights(myNewCards) }
                    } else {
                        await playStep { self.oppDrawFlights(seat: ev.seat, count: ev.count) }
                        seatCountOverride[ev.seat] = new.players.first { $0.seat == ev.seat }?.handCount
                    }
                    deckCountOverride = (deckCountOverride ?? old.deckCount) - ev.count
                }
            }
            // note 39a: the sequence has visibly landed — rest briefly, then
            // swap to results if this bout end was also the game's end.
            if new.isOver { settleResults() }
        }
    }

    /// note 39: hold the board on-screen a beat after whatever's animating
    /// (a bout-end sequence, or an open-delta replay) has visibly finished,
    /// THEN swap to the results screen. The only place `showResults` is ever
    /// set true. A short guard against re-scheduling once it's already flipped.
    private func settleResults() {
        guard !showResults else { return }
        Task {
            try? await Task.sleep(nanoseconds: 500_000_000)
            withAnimation(.easeOut) { showResults = true }
        }
    }

    /// Poll (up to ~1.2s) for a step's frames to be ready, then play it and await
    /// the animation. `build` returns nil (frames not ready — retry), [] (nothing to
    /// animate), or the flights.
    private func playStep(_ build: () -> [Flight]?) async {
        for _ in 0..<26 {
            if let f = build() {
                if !f.isEmpty { await animator.play([f]) }
                return
            }
            try? await Task.sleep(nanoseconds: 45_000_000)
        }
    }

    // MARK: bout-end flight builders (each returns nil until its frames are ready)

    /// `extraCover` (note 17): a cover that ended the bout in the SAME kernel
    /// apply as the discard, so `battles[i].defense` is still nil for its
    /// battle even though the card is really gone — without this it would
    /// just vanish instead of flying to the discard pile like every other
    /// defense.
    private func discardFlights(_ battles: [BattleView], _ frames: [Int: CGRect],
                                extraCover: PendingCover? = nil) -> [Flight]? {
        guard discardFrame != .zero else { return nil }
        var f: [Flight] = []
        for (i, b) in battles.enumerated() {
            guard let rect = frames[i], rect != .zero else { continue }
            f.append(Flight(id: "trash-\(b.attack.identity)", card: b.attack, from: rect, to: discardFrame))
            if let d = b.defense {
                f.append(Flight(id: "trash-\(d.identity)", card: d, from: rect.offsetBy(dx: 8, dy: 6), to: discardFrame))
            } else if let pc = extraCover, pc.battleRect == rect {
                for c in pc.cards {
                    f.append(Flight(id: "trash-\(c.identity)", card: c, from: rect.offsetBy(dx: 8, dy: 6), to: discardFrame))
                }
            }
        }
        return f
    }

    /// note 17: the cover-that-ends-the-bout's own landing step — its cards
    /// fly from their hand rects AT PLAY TIME (snapshotted before `apply`,
    /// since the hand has already moved on by the time this plays) to the
    /// battle they covered. A pure snapshot, so no retry: what was measured
    /// is what there is.
    private func pendingCoverLandingFlights(_ pc: PendingCover) -> [Flight]? {
        pc.cards.enumerated().compactMap { i, c in
            pc.fromRects[c.identity].map {
                Flight(id: "coverland-\(c.identity)", card: c, from: $0,
                      to: pc.battleRect.offsetBy(dx: CGFloat(i) * 8, dy: CGFloat(i) * 6))
            }
        }
    }

    private func pickupToHandFlights(_ battles: [BattleView], _ frames: [Int: CGRect]) -> [Flight]? {
        var pairs: [(card: Card, from: CGRect)] = []
        for (i, b) in battles.enumerated() {
            guard let rect = frames[i], rect != .zero else { continue }
            pairs.append((b.attack, rect))
            if let d = b.defense { pairs.append((d, rect.offsetBy(dx: 8, dy: 6))) }
        }
        guard pairs.allSatisfy({ handCardFrames[$0.card.identity] != nil }) else { return pairs.isEmpty ? [] : nil }
        return pairs.compactMap { p in handCardFrames[p.card.identity].map { Flight(id: "pick-\(p.card.identity)", card: p.card, from: p.from, to: $0) } }
    }

    private func pickupToBadgeFlights(_ battles: [BattleView], _ frames: [Int: CGRect], seat: Int) -> [Flight]? {
        guard let badge = seatFrames[seat], badge != .zero else { return nil }
        var f: [Flight] = []
        for (i, b) in battles.enumerated() {
            guard let rect = frames[i], rect != .zero else { continue }
            f.append(Flight(id: "opick-\(b.attack.identity)", card: b.attack, from: rect, to: badge))
            if let d = b.defense {
                f.append(Flight(id: "opick-\(d.identity)", card: d, from: rect.offsetBy(dx: 8, dy: 6), to: badge))
            }
        }
        return f
    }

    private func myDrawFlights(_ cards: [Card]) -> [Flight]? {
        if cards.isEmpty { return [] }
        guard deckFrame != .zero, cards.allSatisfy({ handCardFrames[$0.identity] != nil }) else { return nil }
        return cards.compactMap { c in handCardFrames[c.identity].map { Flight(id: "draw-\(c.identity)", card: c, from: deckFrame, to: $0) } }
    }

    private func oppDrawFlights(seat: Int, count: Int) -> [Flight]? {
        guard let badge = seatFrames[seat], badge != .zero, deckFrame != .zero else { return nil }
        return (0..<max(count, 1)).map { k in
            Flight(id: "draw-\(seat)-\(k)", card: nil, from: deckFrame, to: badge.offsetBy(dx: CGFloat(k) * 3, dy: 0))
        }
    }

    // game.h LOG_* (packed replay step types) — shared by every helper below
    // AND by MessageTurnController (which resolves the open-delta window
    // BEFORE this view's first paint, notes 6/12) via ReplayDelta.swift, the
    // single source of truth for these values; aliased here so the many
    // `Self.LOG_*` call sites below didn't all need touching.
    private static let LOG_ATTACK = ReplayLogType.attack, LOG_COVER = ReplayLogType.cover
    private static let LOG_PASS = ReplayLogType.pass, LOG_PICKUP = ReplayLogType.pickup
    private static let LOG_DISCARD = ReplayLogType.discard, LOG_DRAW = ReplayLogType.draw

    /// The last bout's refill draws in kernel order: LOG_DRAW (type 9) records after
    /// the most recent discard/pickup, each one player's draw (seat + card count).
    private static func lastBoutDraws(_ replay: DecodedReplay) -> [(seat: Int, count: Int)] {
        var start = 0
        for i in stride(from: replay.logs.count - 1, through: 0, by: -1)
        where replay.logs[i].type == LOG_DISCARD || replay.logs[i].type == LOG_PICKUP {
            start = i + 1; break
        }
        return replay.logs[start...]
            .filter { $0.type == LOG_DRAW }
            .map { (seat: $0.seat, count: max($0.pairs.count, 1)) }
    }

    /// The count effect of ONE delta-replay log entry: how many cards moved
    /// out of the deck, into the discard pile, and into/out of the acting
    /// seat's hand. Shared by the backward (pre-delta) walk and the forward
    /// per-step bump below, so the two can never drift out of sync.
    ///   - LOG_DRAW: deck -n, that seat's hand +n.
    ///   - LOG_DISCARD: discard +pairs.count (seat is -1, a system event).
    ///   - LOG_PICKUP: that seat's hand +pairs.count (the whole table).
    ///   - LOG_ATTACK/LOG_COVER/LOG_PASS: that seat's hand -pairs.count (a
    ///     single card per entry for COVER — game.c logs one GameLog PER
    ///     cover card — but `pairs.count` is 1 there anyway, so this is exact
    ///     either way).
    private static func countDelta(_ ev: ReplayLog) -> (deck: Int, discard: Int, seat: Int?, hand: Int) {
        switch ev.type {
        case LOG_DRAW: let n = max(ev.pairs.count, 1); return (-n, 0, ev.seat, n)
        case LOG_DISCARD: return (0, ev.pairs.count, nil, 0)
        case LOG_PICKUP: return (0, 0, ev.seat, ev.pairs.count)
        case LOG_ATTACK, LOG_COVER, LOG_PASS: return (0, 0, ev.seat, -ev.pairs.count)
        default: return (0, 0, nil, 0)
        }
    }

    /// Deck/discard/every-seat's hand count as of BEFORE `events`, derived by
    /// walking them backward from the FINAL view's counts (the only state
    /// available — there is no per-event snapshot to read here, unlike the
    /// live evwire stream). Only deck/discard and OPPONENT hands are actually
    /// displayed as overrides (mine renders via real cards, not a badge).
    private static func preDeltaCounts(_ events: [ReplayLog], finalView: GameView)
        -> (deck: Int, discard: Int, hand: [Int: Int]) {
        var deck = finalView.deckCount, discard = finalView.discardCount
        var hand = Dictionary(uniqueKeysWithValues: finalView.players.map { ($0.seat, $0.handCount) })
        for ev in events.reversed() {
            let d = countDelta(ev)
            deck -= d.deck; discard -= d.discard
            if let s = d.seat { hand[s, default: 0] -= d.hand }
        }
        return (deck, discard, hand)
    }

    /// note 4: an approximate source rect for a pickup/discard flight replayed
    /// on open — the pre-bout table itself is never rendered (the game is
    /// already past it by the time we open), so there is no real per-battle
    /// rect to fly from. Prefers the last REAL battle rects seen this session
    /// (rare on a fresh open); otherwise a small rect at the board's visual
    /// centre, between the deck/discard corners and the hand, so the flight
    /// still reads as "from the table" rather than from nowhere. nil only
    /// when even the hand hasn't rendered yet (poll again via playStep).
    private func approximateTableCenter() -> CGRect? {
        if !lastBattleFrames.isEmpty {
            let rects = Array(lastBattleFrames.values)
            let midX = rects.map(\.midX).reduce(0, +) / CGFloat(rects.count)
            let midY = rects.map(\.midY).reduce(0, +) / CGFloat(rects.count)
            return CGRect(x: midX - 25, y: midY - 35, width: 50, height: 70)
        }
        guard handFrame != .zero else { return nil }
        let y = deckFrame != .zero ? (deckFrame.midY + handFrame.minY) / 2 : handFrame.minY - 140
        return CGRect(x: handFrame.midX - 25, y: y - 35, width: 50, height: 70)
    }

    /// One delta-replay event's flights. `myCards` are the (already-resolved,
    /// real) cards THIS event contributes to my hand, precomputed by the
    /// caller from `controller.openReplayNewHandCards` — never read off `ev`
    /// itself, since LOG_DRAW/LOG_PICKUP pairs are redacted for any card not
    /// yet publicly played (see `openReplayNewHandCards`'s doc). Returns nil
    /// to ask `playStep` to retry (a needed frame isn't ready yet), or a
    /// (possibly empty) list once it's resolved.
    private func openReplayFlights(_ ev: ReplayLog, view: GameView, myCards: [Card]) -> [Flight]? {
        switch ev.type {
        case Self.LOG_ATTACK, Self.LOG_COVER, Self.LOG_PASS:
            // Mirrors the single-move replay this replaces: best-effort, no
            // retry (the 120ms lead-in before this loop starts is the same
            // grace period the original code relied on).
            let source = ev.seat == controller.mySeat ? handFrame : (seatFrames[ev.seat] ?? .zero)
            var out: [Flight] = []
            for card in ev.pairs.map(\.primary) where !card.isHidden {
                // Skipped (not "not ready") if the card already moved on to a
                // discard/pickup LATER in this same delta — that event flies
                // it instead.
                guard let idx = view.battles.firstIndex(where: { $0.attack == card || $0.defense == card }),
                      let rect = battleFrames[idx] else { continue }
                let from = source != .zero ? source : rect.offsetBy(dx: 0, dy: -220)
                out.append(Flight(id: "open-\(card.identity)-\(ev.type)", card: card, from: from, to: rect))
            }
            return out

        case Self.LOG_DRAW:
            let n = max(ev.pairs.count, 1)
            if ev.seat == controller.mySeat {
                guard deckFrame != .zero, myCards.allSatisfy({ handCardFrames[$0.identity] != nil })
                else { return myCards.isEmpty ? [] : nil }
                return myCards.compactMap { c in handCardFrames[c.identity].map {
                    Flight(id: "opendraw-\(c.identity)", card: c, from: deckFrame, to: $0) } }
            }
            guard let badge = seatFrames[ev.seat], badge != .zero, deckFrame != .zero else { return nil }
            return (0..<n).map { k in
                Flight(id: "opendraw-\(ev.seat)-\(n)-\(k)", card: nil, from: deckFrame,
                      to: badge.offsetBy(dx: CGFloat(k) * 3, dy: 0)) }

        case Self.LOG_PICKUP:
            guard let center = approximateTableCenter() else { return nil }
            let n = max(ev.pairs.count, 1)
            if ev.seat == controller.mySeat {
                guard myCards.allSatisfy({ handCardFrames[$0.identity] != nil })
                else { return myCards.isEmpty ? [] : nil }
                return myCards.compactMap { c in handCardFrames[c.identity].map {
                    Flight(id: "openpick-\(c.identity)", card: c, from: center, to: $0) } }
            }
            guard let badge = seatFrames[ev.seat], badge != .zero else { return nil }
            return (0..<n).map { k in
                Flight(id: "openpick-\(ev.seat)-\(k)", card: nil, from: center,
                      to: badge.offsetBy(dx: CGFloat(k) * 3, dy: 0)) }

        case Self.LOG_DISCARD:
            guard let center = approximateTableCenter(), discardFrame != .zero else { return nil }
            // Backs only (an approximation already — see approximateTableCenter
            // — so this doesn't also claim to know which face was which).
            return ev.pairs.indices.map { i in
                Flight(id: "opendiscard-\(i)", card: nil, from: center, to: discardFrame)
            }

        default:
            return []   // LOG_GOOD/LOG_DEFENDER_CHANGE/LOG_PLAYER_OUT/LOG_GAME_START: no flight.
        }
    }

    /// On opening a delivered bubble, replay everything that happened since I
    /// last looked (notes 4/9/38), as ORDERED sequential animator steps — one
    /// per log entry, using the same `playStep`/`animator.play` machinery the
    /// interactive bout-end sequence uses (so HARNESS_AUTOGAME's
    /// `BoardAnimator.isSequencing` wait still covers it).
    private func replayLastMoveOnOpen(_ view: GameView) {
        // Fresh genesis deal (no prior chain to diff against — genesis
        // controllers never carry a `prevPayload`): fly the opening hand from
        // the deck, the web's deal, through the same sequencer.
        if controller.isGenesis {
            guard let hand = view.me?.hand, !hand.isEmpty else {
                if view.isOver { showResults = true }   // note 39c: nothing to animate
                return
            }
            animator.preHide(Set(hand.map(\.identity)))   // note 36
            Task {
                BoardAnimator.sequenceDepth += 1
                defer { BoardAnimator.sequenceDepth -= 1; animator.clearPreHidden() }
                await playStep { self.myDrawFlights(hand) }
                if view.isOver { settleResults() }
            }
            return
        }

        // notes 6/12: pre-hide EVERY card identity this open's delta will
        // touch — my own hand's new cards (draw/pickup, note 36) AND every
        // attack/cover/pass card the delta lands on the table — all
        // SYNCHRONOUSLY, before this function returns to the `onChange`
        // callback that invoked it, i.e. before `boardContent`'s first real
        // paint. `controller` already resolved the whole delta in `begin()`
        // (`openReplayEvents`/`openReplayTouchedCardIds`) — that's what makes
        // this possible without an async recompute here. The OLD code could
        // only widen ITS OWN hidden set (myNewCardIds only, never battle
        // cards) from inside the Task below, after a 120ms sleep and an
        // awaited kernel call — by which point SwiftUI had already painted
        // the battle grid once with the cover already landed and rotated
        // (`FBattleGrid.pair`'s `coverLanded` reads `!hidden.contains`): the
        // "starts rotated, un-rotates, re-rotates" bug (note 6), and "every
        // cover in a multi-cover shows landed at once, then animates one at a
        // time" (note 12).
        let touchedIds = controller.openReplayTouchedCardIds
        if !touchedIds.isEmpty { animator.preHide(touchedIds) }

        Task {
            BoardAnimator.sequenceDepth += 1
            defer { BoardAnimator.sequenceDepth -= 1; animator.clearPreHidden() }
            // Let the battle slots (and everything else) render + publish
            // their rects first — the event list itself no longer needs this
            // wait: it's `controller.openReplayEvents`, already resolved in
            // `begin()`, not recomputed here.
            try? await Task.sleep(nanoseconds: 120_000_000)
            let events = controller.openReplayEvents
            guard !events.isEmpty else {
                if view.isOver { showResults = true }   // note 39c: nothing to animate
                return
            }

            // Attribute my new cards to MY draw/pickup events in order. The
            // exact split has no visible effect (only the per-step COUNT and
            // the overall real-card SET matter) — precomputed once, up front,
            // so `openReplayFlights` never mutates shared state across
            // `playStep`'s retries.
            //
            // KNOWN LIMITATION: `openReplayNewHandCards` is only populated
            // when `openReplayFromLog` came from an actual cached previous
            // chain (see MessageTurnController.begin()) — the no-cache
            // fallback above has no "before my hand" reference to diff, so
            // `pool` is empty there and MY OWN draw/pickup steps play no
            // flight (an opponent's still do, since those render as backs and
            // need no real identity). A fresh install / cleared cache is the
            // only time this applies; a normal re-open always has the cache.
            var pool = controller.openReplayNewHandCards
            var myCardsByEventIndex: [Int: [Card]] = [:]
            for (i, ev) in events.enumerated()
            where ev.seat == controller.mySeat && (ev.type == Self.LOG_DRAW || ev.type == Self.LOG_PICKUP) {
                let n = min(max(ev.pairs.count, 1), pool.count)
                myCardsByEventIndex[i] = Array(pool.prefix(n))
                pool.removeFirst(n)
            }

            // Freeze deck/discard/opponent-badge counts at their pre-delta
            // values (mirrors flyBoutEndToDiscard), bumping after each step.
            let pre = Self.preDeltaCounts(events, finalView: view)
            deckCountOverride = pre.deck
            discardCountOverride = pre.discard
            for (seat, count) in pre.hand where seat != controller.mySeat { seatCountOverride[seat] = count }
            defer { deckCountOverride = nil; discardCountOverride = nil; seatCountOverride = [:] }

            for (i, ev) in events.enumerated() {
                await playStep { self.openReplayFlights(ev, view: view, myCards: myCardsByEventIndex[i] ?? []) }
                let d = Self.countDelta(ev)
                deckCountOverride = (deckCountOverride ?? view.deckCount) + d.deck
                discardCountOverride = (discardCountOverride ?? view.discardCount) + d.discard
                if let s = d.seat, s != controller.mySeat {
                    seatCountOverride[s] = (seatCountOverride[s] ?? (view.player(s)?.handCount ?? 0)) + d.hand
                }
            }

            // note 39: REMOVED the old `!controller.isOver` guard that used to
            // open this whole function — a receiver opening a decodable
            // FINISHED chain now animates its final move too, then settles.
            // (The FINISHED bubble's own URL still can't be decoded at all
            // today — §12's replay-link swap — so this only helps a chain
            // that DOES decode, e.g. the sender's own board, or a cached chain
            // that reaches FINISHED via this same delta; batch 6 addresses
            // the bubble URL itself.)
            if view.isOver { settleResults() }
        }
    }

    /// notes 33/34: FHandFan already delivers the live boardSpace point on
    /// every change — kept now (previously discarded at the call site) so the
    /// verb hint / ghost-slot preview can resolve the same drop target live.
    private func onDragChanged(_ card: Card, at point: CGPoint) {
        dragCard = card
        dragPoint = point
    }

    private func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        dragCard = nil
        dragPoint = nil
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handFrame)
        if target == .hand { return }   // dropped back in the fan — cancel
        playAt(target, playCards(for: card, view), view)
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
            // Selection-aware: with cards selected, the defender's Take and the
            // attacker's Good must disappear — a stray tap on either while mid-
            // selection would abandon the cards you'd picked (web parity TODO).
            canPickup: acting && has(.pickup) && cards.isEmpty,
            canDone: acting && CardPlay.canSayGood(battles: view.battles, legal: controller.legal) && cards.isEmpty,
            canUndo: controller.canSend,
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onUndo: { Task {
                await controller.undo()
                // Undo that empties `pending` leaves `stageNow()` a no-op (canStage
                // goes false) — but the host still holds the PREVIOUSLY staged
                // bubble/payload. Retract it explicitly instead of leaving it lit.
                if controller.canStage { await stageNow() } else { onUnstage() }
            } }
        )
    }

    private func hand(_ view: GameView) -> some View {
        FHandFan(cards: view.me?.hand ?? [], trumpSuit: view.trumpSuit,
                 selection: $selection, onTap: { toggle($0) },
                 onDragChanged: { card, point in onDragChanged(card, at: point) },
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
        // note 17: a cover might end the bout in the SAME kernel apply as the
        // cover itself (the defender's hand empties) — stash enough, BEFORE
        // applying, for flyBoutEndToDiscard to synthesize the landing step it
        // would otherwise have no rendered state to animate from.
        if move.type == .cover, case .battle(let i) = target, i >= 0, i < view.battles.count,
           let rect = battleFrames[i] ?? lastBattleFrames[i] {
            pendingCover = PendingCover(
                cards: cards, battleRect: rect,
                fromRects: handCardFrames.filter { pair in cards.contains { $0.identity == pair.key } })
        } else {
            pendingCover = nil
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

/// The first-attacker sword — a hand-built UPRIGHT sword on a 24x24 grid that
/// actually reads as a sword: a pointed blade, a wide crossguard, a grip, and a
/// round pommel, all filled FLAT dark gray. Marks "you open this bout".
struct FSword: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            func R(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
                CGRect(x: x * s, y: y * s, width: w * s, height: h * s)
            }
            let gray = Color(hex: 0x3A3A3A)

            // Blade: a pointed spike from the tip (top) down to the guard.
            var blade = Path()
            blade.move(to: P(12, 1.5))     // tip
            blade.addLine(to: P(13.5, 5.5))
            blade.addLine(to: P(13.5, 14.5))
            blade.addLine(to: P(10.5, 14.5))
            blade.addLine(to: P(10.5, 5.5))
            blade.closeSubpath()
            ctx.fill(blade, with: .color(gray))

            // Crossguard: a wide bar under the blade.
            ctx.fill(Path(roundedRect: R(6, 14.3, 12, 2.2), cornerRadius: 0.7 * s), with: .color(gray))
            // Grip: the handle below the guard.
            ctx.fill(Path(R(10.9, 16.4, 2.2, 4.6)), with: .color(gray))
            // Pommel: a round knob at the base.
            let r = 1.7 * s
            ctx.fill(Path(ellipseIn: CGRect(x: 12 * s - r, y: 21.2 * s - r, width: 2 * r, height: 2 * r)),
                     with: .color(gray))
        }
        .frame(width: size, height: size)
        .rotationEffect(.degrees(45))   // point it up-and-to-the-right
        .accessibilityLabel(Text("You attack first"))
    }
}

/// The defender shield — a hand-built heraldic shield (flat top, straight upper
/// sides curving to a point at the bottom), filled flat LIGHT gray with a darker
/// edge for definition on the wool. Marks the current defender.
struct FShield: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            let gray = Color(hex: 0xCACFD4), edge = Color(hex: 0x5E6368)
            var shield = Path()
            shield.move(to: P(4, 3.5))
            shield.addLine(to: P(20, 3.5))
            shield.addLine(to: P(20, 11))
            shield.addQuadCurve(to: P(12, 21.5), control: P(20, 18))
            shield.addQuadCurve(to: P(4, 11), control: P(4, 18))
            shield.closeSubpath()
            ctx.fill(shield, with: .color(gray))
            ctx.stroke(shield, with: .color(edge), style: StrokeStyle(lineWidth: 1.1 * s, lineJoin: .round))
        }
        .frame(width: size, height: size)
        .accessibilityLabel(Text("Defending"))
    }
}

/// The "said good" mark — a hand-built green check stroke on the same 24x24 grid
/// as FSword/FShield. Hand-built for the same reason: SF Symbols (previously
/// `checkmark.seal.fill`) are unreliable under ImageRenderer bubble snapshots.
struct FCheck: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            let green = Color(hex: 0x2E9E4F)
            var check = Path()
            check.move(to: P(4.5, 12.5))
            check.addLine(to: P(9.5, 18))
            check.addLine(to: P(20, 5.5))
            ctx.stroke(check, with: .color(green),
                       style: StrokeStyle(lineWidth: 3 * s, lineCap: .round, lineJoin: .round))
        }
        .frame(width: size, height: size)
        .accessibilityLabel(Text("Good"))
    }
}

/// One row of the ranked end screen (web WinScreen parity, minus ELO). `place` is
/// 1-based: rank 1 is the first player out (best); the fool is `place == total`.
struct FinishRow: Identifiable {
    let place: Int
    let total: Int
    let name: String
    let isYou: Bool
    var id: Int { place }
    var isFool: Bool { place == total }
}

/// The game-over results, replacing the board when the fool is decided (design:
/// mirror the web WinScreen). Players are listed in finishing order - first out
/// (rank 1) down to the fool - with New game at the bottom so any player, out or
/// not, can deal the next one. No ELO in the iMessage game, and no emoji: the rank
/// is a colored number (brass for 1st, red for the fool) with a "Fool" tag.
struct FGameOverList: View {
    private static let rowH: CGFloat = 34   // fixed per-row height (plank scales with player count)
    let rows: [FinishRow]
    let onNewGame: () -> Void

    private var plankHeight: CGFloat { CGFloat(rows.count) * Self.rowH }

    var body: some View {
        // Title + ranking sit at the TOP; New game is pinned to the bottom.
        VStack(spacing: 14) {
            Text(FStrings.t("game_over"))
                .font(.title2.weight(.bold))
                .foregroundStyle(FColor.textPrimary)
                .shadow(color: .black.opacity(0.6), radius: 3, y: 1)
            // ONE continuous wood plank behind the whole ranking (no dividers):
            // WoodFill is a ZStack LAYER (not a .background, which over-drew and
            // made the plank too tall), hard-clipped to exactly rows × rowH so it
            // is a single block that scales with the player count.
            ZStack {
                WoodFill()
                VStack(spacing: 0) {
                    ForEach(rows) { row in
                        HStack(spacing: 12) {
                            // The last place reads "Fool" in the rank column itself
                            // (no separate pill); everyone else is "#N".
                            // Rank contrast on brown wood (note 41): 1st stays
                            // brass, the fool is a brighter lifted red (the old
                            // dark red/brown read as near-black on the plank), and
                            // every other place is bone with a dark drop shadow —
                            // the same bone+shadow treatment the board's own text
                            // uses on wool.
                            Text(row.isFool ? FStrings.t("ios.fool") : "#\(row.place)")
                                .font(.headline.weight(.heavy)).monospacedDigit()
                                .foregroundStyle(row.isFool ? Color(hex: 0xD84438)
                                                 : (row.place == 1 ? FColor.win : FColor.card))
                                .shadow(color: .black.opacity(0.5), radius: 1, y: 1)
                                .frame(width: 56, alignment: .leading)
                            Text(row.name + (row.isYou ? " (\(FStrings.t("ios.you")))" : ""))
                                .font(.body.weight(.semibold))
                                .foregroundStyle(.black.opacity(0.88))
                                .lineLimit(1)
                            Spacer(minLength: 0)
                        }
                        .frame(height: Self.rowH)
                        .padding(.horizontal, 12)
                    }
                }
            }
            .frame(height: plankHeight)
            .clipShape(Rectangle())
            .overlay(Rectangle().strokeBorder(.black.opacity(0.4), lineWidth: 1.5))
            .padding(.horizontal, 4)
            Spacer(minLength: 0)
            FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}
