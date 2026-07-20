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
                // New game there.
                if controller.isOver {
                    FGameOverList(rows: finishRows(view), onNewGame: onNewGame)
                } else {
                    boardContent(view)
                }
            } else {
                ProgressView()
            }
        }
        .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)   // top margin so the ring isn't clipped in the compact drawer
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
        .onPreferenceChange(SeatFramesKey.self) { seatFrames = $0 }
        .onPreferenceChange(HandCardFramesKey.self) { handCardFrames = $0 }
        .onChange(of: controller.view) { flyBoutEndToDiscard(to: $0) }
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
                    .offset(y: -30)   // snug into the top-left corner (the well has empty space above the stack)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                FDiscardPile(count: discardCountOverride ?? view.discardCount)
                    .offset(y: -16)   // snug into the top-right corner
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                // First-attacker sword: it's my open (empty table, I'm first
                // attacker). Sits just above my hand - the web PlayerRing sword,
                // replacing the old centred "your move" pill.
                if firstAttackerMustOpen(view) {
                    FSword(size: 30)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                        .padding(.bottom, 86)
                }

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

    /// I must open this bout: the table is empty, I'm the first attacker, and I
    /// have a legal move. Drives the first-attacker sword (web PlayerRing).
    private func firstAttackerMustOpen(_ view: GameView) -> Bool {
        view.battles.isEmpty && view.firstAttacker == controller.mySeat && controller.iCanAct
    }

    private func battlesArea(_ view: GameView) -> some View {
        Group {
            if view.battles.isEmpty {
                // Empty table: render nothing (web parity). A "no battle" label just
                // tells the player what they can already see (owner's call).
                Color.clear
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
        guard !reduceMotion, let new = newView else { return }
        // First time the board appears with a delivered game: replay the last move.
        if prior == nil { replayLastMoveOnOpen(new); return }
        guard let old = prior, !old.battles.isEmpty, new.battles.isEmpty else { return }

        let beaten = new.discardCount > old.discardCount
        let oldBattles = old.battles
        let boutFrames = lastBattleFrames
        let oldHandIds = Set((old.me?.hand ?? []).map(\.identity))
        let myNewCards = (new.me?.hand ?? []).filter { !oldHandIds.contains($0.identity) }
        // The pickup taker (nil when beaten): the seat whose hand grew by the table.
        let takerSeat: Int? = beaten ? nil
            : new.players.first { p in p.handCount > (old.players.first { $0.seat == p.seat }?.handCount ?? 0) }?.seat

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
            }

            // STEP 1 — the table clears; its count bumps only AFTER the flight lands.
            if beaten {
                await playStep { self.discardFlights(oldBattles, boutFrames) }
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
            guard let replay = await MessageKernel.shared.residentReplay() else { return }
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

    private func discardFlights(_ battles: [BattleView], _ frames: [Int: CGRect]) -> [Flight]? {
        guard discardFrame != .zero else { return nil }
        var f: [Flight] = []
        for (i, b) in battles.enumerated() {
            guard let rect = frames[i], rect != .zero else { continue }
            f.append(Flight(id: "trash-\(b.attack.identity)", card: b.attack, from: rect, to: discardFrame))
            if let d = b.defense {
                f.append(Flight(id: "trash-\(d.identity)", card: d, from: rect.offsetBy(dx: 8, dy: 6), to: discardFrame))
            }
        }
        return f
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

    /// The last bout's refill draws in kernel order: LOG_DRAW (type 9) records after
    /// the most recent discard/pickup, each one player's draw (seat + card count).
    private static func lastBoutDraws(_ replay: DecodedReplay) -> [(seat: Int, count: Int)] {
        let LOG_PICKUP = 4, LOG_DISCARD = 6, LOG_DRAW = 9
        var start = 0
        for i in stride(from: replay.logs.count - 1, through: 0, by: -1)
        where replay.logs[i].type == LOG_DISCARD || replay.logs[i].type == LOG_PICKUP {
            start = i + 1; break
        }
        return replay.logs[start...]
            .filter { $0.type == LOG_DRAW }
            .map { (seat: $0.seat, count: max($0.pairs.count, 1)) }
    }

    /// On opening a delivered bubble, replay the last card-placing move: fly the
    /// card(s) it put on the table into their battle slots FROM THE PLAYER WHO
    /// PLAYED THEM - the actor's seat badge (an opponent) or my hand (my own move)
    /// - so the direction reflects who attacked, not a generic "from the top"
    /// (which, in a 2p game, made every attack look like the opponent's). Reads the
    /// last LOG_ATTACK/LOG_COVER (and its actor seat) from the packed replay stream.
    private func replayLastMoveOnOpen(_ view: GameView) {
        guard !reduceMotion, !controller.isOver else { return }
        // Fresh genesis deal (no moves yet): fly the opening hand from the deck —
        // the web's deal, one step through the same sequencer.
        if view.battles.isEmpty {
            if controller.isGenesis, let hand = view.me?.hand, !hand.isEmpty {
                Task {
                    BoardAnimator.sequenceDepth += 1
                    defer { BoardAnimator.sequenceDepth -= 1 }
                    await playStep { self.myDrawFlights(hand) }
                }
            }
            return
        }
        Task {
            BoardAnimator.sequenceDepth += 1
            defer { BoardAnimator.sequenceDepth -= 1 }
            // Let the battle slots render + publish their rects first.
            try? await Task.sleep(nanoseconds: 120_000_000)
            guard let replay = await MessageKernel.shared.residentReplay() else { return }
            let LOG_ATTACK = 1, LOG_COVER = 2   // game.h LOG_* (packed step types)
            guard let last = replay.logs.last(where: { $0.type == LOG_ATTACK || $0.type == LOG_COVER })
            else { return }
            // Where the card comes FROM: the actor's seat badge if it's someone
            // else, my hand if it was me. Fall back to a downward slide only if
            // that rect hasn't been measured yet.
            let actorSource = last.seat == controller.mySeat ? handFrame : (seatFrames[last.seat] ?? .zero)
            var flights: [Flight] = []
            for card in last.pairs.map(\.primary) where !card.isHidden {
                guard let idx = view.battles.firstIndex(where: { $0.attack == card || $0.defense == card }),
                      let rect = battleFrames[idx] else { continue }
                let from = actorSource != .zero ? actorSource : rect.offsetBy(dx: 0, dy: -220)
                flights.append(Flight(id: "open-\(card.identity)", card: card, from: from, to: rect))
            }
            guard !flights.isEmpty else { return }
            await animator.play([flights])
        }
    }

    private func onDragChanged(_ card: Card) { dragCard = card }

    private func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        dragCard = nil
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
                            Text(row.isFool ? FStrings.t("ios.fool") : "#\(row.place)")
                                .font(.headline.weight(.heavy)).monospacedDigit()
                                .foregroundStyle(row.isFool ? Color(hex: 0x8A1810)
                                                 : (row.place == 1 ? Color(hex: 0x5A3B00) : .black.opacity(0.55)))
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
