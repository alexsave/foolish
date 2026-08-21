// TableView.swift — the product screen (§6 screen 3). One layout drives offline
// (LocalGame) today and online (OnlineGame) after the D refactor. Zones per
// §16.B3: opponents strip (top), battles (center), deck well (trailing), own fan
// (bottom), action bar (above the fan). Chrome-free while the round is live —
// status communicates through the board, not banners (§5.5).
//
// Interaction is the ONE pattern in the app (§16.B3): tap a card / battle →
// consult the kernel's legal menu (humanLegal) → play the matching move, or a
// rigid reject. No Durak rule is decided here (§3).

import SwiftUI

public struct TableView<Session: GameSession>: View {
    @ObservedObject var game: Session
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var selection: Set<String> = []
    @State private var toast: String?
    // Drag-to-play state (frames published by FBattleGrid/FHandFan in `boardSpace`).
    @State private var battleFrames: [Int: CGRect] = [:]
    @State private var handFrame: CGRect = .zero
    @State private var dragCard: Card?
    /// Shared card-flight namespace: a card keeps its identity moving hand→table.
    @Namespace private var cardNS

    /// Called when the local player asks to leave a live game (confirmed upstream).
    public let onLeave: () -> Void

    public init(game: Session, onLeave: @escaping () -> Void) {
        self.game = game
        self.onLeave = onLeave
    }

    public var body: some View {
        GeometryReader { geo in
            ZStack {
                tableBackground
                if let view = game.view {
                    // The felt is one round table: opponents sit on a ring, the
                    // battle/deck well is its hub, and the local player IS the
                    // fan along the bottom edge (§6 screen 3; the web PlayerRing
                    // seats every player on a circle, self at the bottom — here
                    // the fan takes self's seat). Everything shares one ZStack so
                    // a card can fly between any two zones under one geometry.
                    // Battles are the hub; the deck well pins top-left with the
                    // flipped trump, the discard pile top-right (§16.B3 / the web
                    // layout). Counts only for discard.
                    battlesHub(view)
                        .fixedSize()
                        .position(x: geo.size.width / 2, y: geo.size.height * ringCenterYFraction)

                    // FDeckWell now anchors its content top-leading inside its
                    // 92x108 frame (batch-2 equal-inset change); this .position
                    // centers that frame, so nudge it down to keep the stack's
                    // visual spot (content center moved ~16pt up within the frame).
                    //
                    // Batch 11 (notes 1/10/14) re-anchored FDeckWell's internal
                    // stock to its BOTTOM card's own rotated corner instead of a
                    // union-of-all-layers box (see FDeckWell's type doc), which
                    // moves that internal visual centre a little further up/left
                    // than it was when this +16 was eyeballed. This .position()
                    // call is a DIFFERENT layout mechanism (centring the whole
                    // 92x108 frame) than the corner-pinned `.frame(alignment:)`
                    // MessageTableView uses, so nothing here needed to change for
                    // notes 1/10 to hold in the iMessage board — but this +16 is
                    // now a slightly stale eyeball on the OFFLINE app board
                    // specifically, and could stand a fresh look against a live
                    // screenshot of THIS screen in a future pass.
                    deckWell(view)
                        .position(x: geo.size.width * 0.14, y: geo.size.height * 0.14 + 16)

                    if view.discardCount > 0 {
                        discardPile(view)
                            .position(x: geo.size.width * 0.86, y: geo.size.height * 0.14)
                    }

                    opponentsRing(view, in: geo.size)

                    VStack(spacing: 0) {
                        Spacer(minLength: 0)
                        actionBar(view)
                            .padding(.bottom, FSpace.s)
                        handZone(view)
                            .frame(height: geo.size.height * 0.22)
                            .padding(.bottom, FSpace.l)
                    }
                    .frame(maxHeight: .infinity, alignment: .bottom)
                } else {
                    ProgressView().tint(FColor.textPrimary)
                }
            }
            // Animate the whole board on a state change so a card flies between the
            // hand and the battle hub (matchedGeometry across zones), not just the
            // fan reflowing.
            .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: game.view)
        }
        .coordinateSpace(name: boardSpace)
        .onPreferenceChange(BattleFramesKey.self) { battleFrames = $0 }
        .onPreferenceChange(HandFrameKey.self) { handFrame = $0 }
        .fToast($toast, accent: true)
        .onChange(of: game.lastReject) { reject in
            guard reject != nil else { return }
            Haptics.fire(.reject)
            toast = FStrings.t("ios.reject")
        }
        .navigationBarBackButtonHidden(true)
    }

    // MARK: zones

    private var tableBackground: some View {
        // Woven wool — the website's table material (§IOS_PHONE_LAYOUT §4) — with
        // the subtle vignette baked into TableBackground.
        TableBackground()
    }

    // The ring geometry (fractions of the table). The hub sits a little above
    // centre so the fan has room along the bottom; opponents ride an ellipse
    // around it. Tuned against the 8-seat worst case so no badge collides with
    // the fan or clips an edge.
    // A flattened ellipse: wide enough to seat 8, short enough that the top-most
    // side seats clear the deck/discard corners (the pure circle put an 8-seat
    // shoulder right on the deck).
    private let ringCenterYFraction: CGFloat = 0.46
    private let ringRadiusXFraction: CGFloat = 0.37
    private let ringRadiusYFraction: CGFloat = 0.30

    /// Opponents on the ellipse. Seat order is preserved and rotated so the
    /// local player's neighbours sit at the bottom corners, exactly like the
    /// web's PlayerRing (`visual_index = (seat - self + n) % n`); self's slot
    /// (index 0, straight down) is left to the hand fan.
    private func opponentsRing(_ view: GameView, in size: CGSize) -> some View {
        let opponents = view.players.filter { $0.seat != game.humanSeat }
        let n = max(view.numPlayers, 2)
        let cx = size.width / 2
        let cy = size.height * ringCenterYFraction
        let rx = size.width * ringRadiusXFraction
        let ry = size.height * ringRadiusYFraction
        return ForEach(opponents) { p in
            let vi = Double((p.seat - game.humanSeat + n) % n)
            let theta = 2 * Double.pi * vi / Double(n)
            let x = cx - CGFloat(sin(theta)) * rx
            let y = cy + CGFloat(cos(theta)) * ry
            seatBadge(p, view).position(x: x, y: y)
        }
    }

    private func seatBadge(_ p: PlayerView, _ view: GameView) -> some View {
        FSeatBadge(
            name: seatName(p),
            handCount: p.handCount,
            isDefender: view.defender == p.seat,
            isAttacker: view.firstAttacker == p.seat,
            saidGood: view.hasSaidGood(p.seat),
            thinking: game.thinking && (game.actorMask & (1 << p.seat)) != 0,
            isOut: p.isOut
        )
    }

    /// Display name for an opponent seat. Offline views carry no names, so the
    /// session's `seatNames` map (localized bot names, §IOS_BOT_NAMING) wins;
    /// online rows carry a raw `%`-nickname that gets localized; else `P<seat>`.
    private func seatName(_ p: PlayerView) -> String {
        if let mapped = game.seatNames[p.seat] { return mapped }
        if !p.name.isEmpty { return BotNames.displayNickname(p.name) }
        return "P\(p.seat)"
    }

    /// The hub of the ring — the seat-free centre of the ellipse. Battles grow
    /// out from here and wrap; the deck and discard live in the top corners.
    private func battlesHub(_ view: GameView) -> some View {
        FBattleGrid(
            battles: view.battles,
            trumpSuit: view.trumpSuit,
            coverable: coverableBattles(view),
            onTapBattle: { idx in tapBattle(idx, view) },
            namespace: cardNS
        )
    }

    /// Stock, top-left: card back + flipped trump laid under it + remaining count.
    private func deckWell(_ view: GameView) -> some View {
        FDeckWell(
            deckCount: view.deckCount,
            flipped: view.flipped,
            hasFlipped: view.hasFlipped,
            trumpSuit: view.trumpSuit
        )
    }

    /// Discard, top-right: a randomly-rotated stack of red backs with the count
    /// centred on it — the web's DiscardPile (counts only; beaten cards are out
    /// of play). Web-layout parity per IOS_APP_DESIGN §17.10.
    private func discardPile(_ view: GameView) -> some View {
        let layers = min(max(view.discardCount, 1), 5)
        return ZStack {
            ForEach(0..<layers, id: \.self) { i in
                FCard(card: nil, backSeed: UInt64(3 + i), size: CGSize(width: 46, height: 66))
                    .rotationEffect(.degrees(discardRotation(i)))
            }
            Text("\(view.discardCount)")
                .font(.system(size: 17, weight: .bold))
                .foregroundColor(.white)
                .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
        }
        .frame(width: 84, height: 84)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(view.discardCount) cards discarded")
    }

    /// Deterministic per-layer tilt (mirrors CardBack's seeded random rotation).
    private func discardRotation(_ i: Int) -> Double {
        let s = sin(Double(42 + i * 1000)) * 10000
        return (s - floor(s)) * 40 - 20
    }

    @ViewBuilder
    private func actionBar(_ view: GameView) -> some View {
        let cards = selectedCards(view)
        let defending = view.defender == game.humanSeat
        FActionBar(
            canAttack: !defending && CardPlay.canAttack(cards, legal: game.humanLegal),
            canCover: defending && CardPlay.canCover(cards, battles: view.battles, legal: game.humanLegal),
            canPass: defending && CardPlay.canPass(cards, legal: game.humanLegal),
            canPickup: CardPlay.has(.pickup, in: game.humanLegal),
            canDone: CardPlay.canSayGood(battles: view.battles, legal: game.humanLegal),
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { game.play(.pickup); selection.removeAll() },
            onDone: { game.play(.good); selection.removeAll() }
        )
    }

    private func handZone(_ view: GameView) -> some View {
        let hand = view.me?.hand ?? []
        return FHandFan(
            cards: hand,
            trumpSuit: view.trumpSuit,
            disabled: game.inFlight,          // Stage C1 in-flight lock (§8.2)
            selection: $selection,
            onTap: { card in toggle(card) },
            onDragChanged: { card, _ in dragCard = card },
            onDragEnded: { card, point in onDragEnded(card, at: point, view) },
            namespace: cardNS
        )
        .padding(.horizontal, FSpace.s)
    }

    /// The cards a play uses: the whole selection if the card is part of it, else
    /// just that card (web selected-or-single).
    private func playCards(for card: Card, _ view: GameView) -> [Card] {
        selection.contains(card.identity) ? selectedCards(view) : [card]
    }

    private func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        dragCard = nil
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handFrame)
        if target == .hand { return }   // dropped back in the fan — cancel
        playAt(target, playCards(for: card, view), view)
    }

    // MARK: interaction — dumb selection; CardPlay resolves (selection, target)
    // into one legal move (mirrors the web's determineGameAction). Every decision
    // reads game.humanLegal — the kernel menu — never a hand-rolled rule.

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

    /// Resolve the selection against a drop/tap target and play it, or reject.
    private func playAt(_ target: PlayTarget, _ cards: [Card], _ view: GameView) {
        guard let move = CardPlay.resolve(cards: cards, target: target,
                                          isDefender: view.defender == game.humanSeat,
                                          battles: view.battles, legal: game.humanLegal) else {
            Haptics.fire(.reject); toast = FStrings.t("ios.reject"); return
        }
        game.play(move); selection.removeAll()
    }

    /// Cover button: cover the first uncovered attack the selection can beat.
    private func playCover(_ cards: [Card], _ view: GameView) {
        guard let i = CardPlay.coverableBattles(cards: cards, battles: view.battles,
                                                legal: game.humanLegal).sorted().first else {
            Haptics.fire(.reject); return
        }
        playAt(.battle(i), cards, view)
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        let cards = dragCard.map { playCards(for: $0, view) } ?? selectedCards(view)
        return CardPlay.coverableBattles(cards: cards, battles: view.battles, legal: game.humanLegal)
    }
}
