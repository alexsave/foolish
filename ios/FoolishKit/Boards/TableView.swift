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
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var selection: Set<String> = []
    @State private var toast: String?

    // Drag-to-play state (§ redesign). The finger location is tracked in the
    // "table" coordinate space; battle frames are captured there too so we can
    // tell which battle a card is dropped on.
    private static var space: String { "table" }
    @State private var dragCard: Card?
    @State private var dragLoc: CGPoint = .zero
    @State private var dragAction: DropAction = .none
    @State private var battleFrames: [Int: CGRect] = [:]
    @State private var viewSize: CGSize = .zero

    enum DropAction: Equatable { case none, attack, cover(Int), pass }

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
                    let opponents = view.players.filter { $0.seat != game.humanSeat }

                    // Opponents arced across the top so a full 8-seat table fits
                    // even on a small phone (the web's ring, flattened for portrait).
                    ForEach(Array(opponents.enumerated()), id: \.element.seat) { pair in
                        seatBadge(pair.element, view)
                            .position(arcPosition(pair.offset, count: opponents.count, in: geo.size))
                    }

                    // Deck + flipped trump, pinned to the top-left corner.
                    deckWell(view)
                        .position(x: FSpace.xxl + 24, y: geo.safeAreaInsets.top + 62)

                    // The battles, centred in the open middle of the table.
                    battlesZone(view)
                        .frame(maxWidth: geo.size.width - FSpace.xl * 2)
                        .position(x: geo.size.width / 2, y: geo.size.height * 0.46)

                    // Action plaques + the player's hand, anchored to the bottom.
                    VStack(spacing: FSpace.s) {
                        actionBar(view)
                        handZone(view).frame(height: 132)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .padding(.bottom, geo.safeAreaInsets.bottom + FSpace.s)
                    .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: view)
                } else {
                    ProgressView().tint(FColor.textPrimary)
                }

                // Drag shadow: the card in flight + an action pill, following the
                // finger. Non-interactive so it never eats the gesture.
                if let card = dragCard {
                    dragShadow(card)
                        .position(dragLoc)
                        .allowsHitTesting(false)
                        .transition(.opacity)
                }
            }
            .coordinateSpace(name: Self.space)
            .onPreferenceChange(BattleFramesKey.self) { battleFrames = $0 }
            .onAppear { viewSize = geo.size }
            .onChange(of: geo.size) { viewSize = $0 }
        }
        .fToast($toast, accent: true)
        .onChange(of: game.lastReject) { reject in
            guard reject != nil else { return }
            Haptics.fire(.reject)
            toast = FStrings.t("ios.reject")
        }
        .navigationBarBackButtonHidden(true)
    }

    /// Position for opponent `i` of `count` along a shallow top arc, kept clear
    /// of the top-left deck corner.
    private func arcPosition(_ i: Int, count: Int, in size: CGSize) -> CGPoint {
        let leftMargin: CGFloat = 64, rightMargin: CGFloat = 52
        let usableW = max(0, size.width - leftMargin - rightMargin)
        let t = count <= 1 ? 0.5 : CGFloat(i) / CGFloat(count - 1)
        let x = leftMargin + t * usableW
        let baseY: CGFloat = 178
        let arch: CGFloat = count <= 1 ? 0 : 34
        let y = baseY - sin(Double(t) * .pi) * arch
        return CGPoint(x: x, y: y)
    }

    private func seatBadge(_ p: PlayerView, _ view: GameView) -> some View {
        FSeatBadge(
            name: p.name.isEmpty ? "P\(p.seat)" : p.name,
            handCount: p.handCount,
            isDefender: view.defender == p.seat,
            isAttacker: view.firstAttacker == p.seat,
            saidGood: view.hasSaidGood(p.seat),
            thinking: game.thinking && (game.actorMask & (1 << p.seat)) != 0,
            isOut: p.isOut
        )
    }

    private func deckWell(_ view: GameView) -> some View {
        FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                  hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
    }

    // MARK: zones

    private var tableBackground: some View {
        // Woven wool table (redesign): the material + its warm vignette.
        WoolBackground()
    }

    private func battlesZone(_ view: GameView) -> some View {
        FBattleGrid(
            battles: view.battles,
            trumpSuit: view.trumpSuit,
            coverable: coverableBattles(view),
            activeTarget: { if case let .cover(i) = dragAction { return i }; return nil }(),
            coordinateSpace: Self.space,
            onTapBattle: { idx in tapBattle(idx, view) }
        )
    }

    /// The card in flight during a drag, plus a pill naming the pending action.
    @ViewBuilder
    private func dragShadow(_ card: Card) -> some View {
        VStack(spacing: 6) {
            if let label = actionLabel(dragAction) {
                Text(label)
                    .font(FType.title(14))
                    .foregroundColor(FColor.textPrimary)
                    .padding(.horizontal, FSpace.m).padding(.vertical, 5)
                    .background(Capsule().fill(Color.black.opacity(0.8)))
                    .overlay(Capsule().strokeBorder(pillColor(dragAction), lineWidth: 1.5))
            }
            FCard(card: card,
                  trump: game.view?.trumpSuit != nil && card.suit == game.view?.trumpSuit,
                  size: CGSize(width: 66, height: 94))
                .shadow(color: .black.opacity(0.4), radius: 8, x: 0, y: 6)
                .scaleEffect(1.04)
        }
    }

    private func actionLabel(_ a: DropAction) -> String? {
        switch a {
        case .attack: return FStrings.t("attack")
        case .cover: return FStrings.t("cover")
        case .pass:  return FStrings.t("pass")
        case .none:  return nil
        }
    }
    private func pillColor(_ a: DropAction) -> Color {
        a == .none ? .clear : FColor.win
    }

    @ViewBuilder
    private func actionBar(_ view: GameView) -> some View {
        FActionBar(
            canPickup: humanCan(.pickup),
            canDone: humanCan(.good),
            canTransfer: transferMove(view) != nil,
            onPickup: { game.play(.pickup); selection.removeAll() },
            onDone: { game.play(.good); selection.removeAll() },
            onTransfer: { if let m = transferMove(view) { game.play(m); selection.removeAll() } }
        )
    }

    private func handZone(_ view: GameView) -> some View {
        let hand = view.me?.hand ?? []
        return FHandFan(
            cards: hand,
            trumpSuit: view.trumpSuit,
            disabled: game.inFlight,          // Stage C1 in-flight lock (§8.2)
            selection: $selection,
            onTap: { card in tapCard(card, view) },
            dragSpace: Self.space,
            onDragChanged: { card, loc in
                dragCard = card
                dragLoc = loc
                dragAction = dropAction(card, at: loc, view: view)
            },
            onDragEnded: { card, loc in
                let action = dropAction(card, at: loc, view: view)
                execute(action, card: card, view: view)
                dragCard = nil
                dragAction = .none
            }
        )
        .padding(.horizontal, FSpace.s)
    }

    // MARK: drag-to-play (mirrors the web DragContext: drop on a battle to cover,
    // drop on the open table to attack, or drop as a defender to pass)

    private func dropAction(_ card: Card, at loc: CGPoint, view: GameView) -> DropAction {
        // Over a coverable battle? (a little slop so the edge is forgiving)
        for (idx, frame) in battleFrames where frame.insetBy(dx: -14, dy: -14).contains(loc) {
            if coverMove(card, battleIndex: idx, view) != nil { return .cover(idx) }
        }
        // Still down in the hand zone → not a play (rearrange/cancel).
        if loc.y > handTopY { return .none }
        // Up on the open table: attacker attacks, defender passes.
        if attackMove(card, view) != nil { return .attack }
        if passMove(card, view) != nil { return .pass }
        return .none
    }

    /// The y below which we treat a drag as "still in the hand" (bottom band) —
    /// releasing here is a cancel, not a play.
    private var handTopY: CGFloat { viewSize.height > 0 ? viewSize.height - 150 : 100_000 }

    private func attackMove(_ card: Card, _ view: GameView) -> Move? {
        game.humanLegal.first { $0.type == .attack && $0.cards == [card] }
    }
    private func coverMove(_ card: Card, battleIndex idx: Int, _ view: GameView) -> Move? {
        guard idx < view.battles.count else { return nil }
        let attack = view.battles[idx].attack
        return game.humanLegal.first { $0.type == .cover && $0.cards == [card] && ($0.attackCards ?? []) == [attack] }
    }
    private func passMove(_ card: Card, _ view: GameView) -> Move? {
        game.humanLegal.first { $0.type == .pass && $0.cards.contains(card) }
    }

    private func execute(_ action: DropAction, card: Card, view: GameView) {
        let move: Move?
        switch action {
        case .attack:        move = attackMove(card, view)
        case .cover(let i):  move = coverMove(card, battleIndex: i, view)
        case .pass:          move = passMove(card, view)
        case .none:          move = nil
        }
        if let move { Haptics.fire(.drop); game.play(move); selection.removeAll() }
        else if action == .none { /* snap back silently */ }
    }

    // MARK: interaction (all decisions consult humanLegal — the kernel menu)

    private func tapCard(_ card: Card, _ view: GameView) {
        // Attacker path: a single-card attack with exactly this card → play now.
        if let attack = game.humanLegal.first(where: {
            $0.type == .attack && $0.cards == [card]
        }) {
            game.play(attack); selection.removeAll(); return
        }
        // Defender path: selecting a card that can cover something → arm it and
        // wait for the target battle tap. If it can't do anything, reject.
        let canCover = game.humanLegal.contains { $0.type == .cover && $0.cards == [card] }
        let canTransfer = game.humanLegal.contains { $0.type == .pass && $0.cards.contains(card) }
        if canCover || canTransfer {
            selection = [card.identity]
        } else {
            Haptics.fire(.reject)
            toast = FStrings.t("ios.reject")
        }
    }

    private func tapBattle(_ index: Int, _ view: GameView) {
        guard index < view.battles.count else { return }
        guard let selId = selection.first,
              let card = view.me?.hand?.first(where: { $0.identity == selId }) else { return }
        let attack = view.battles[index].attack
        if let cover = game.humanLegal.first(where: {
            $0.type == .cover && $0.cards == [card] && ($0.attackCards ?? []) == [attack]
        }) {
            game.play(cover); selection.removeAll()
        } else {
            Haptics.fire(.reject)
        }
    }

    // MARK: legal-menu helpers (thin reads of the kernel menu)

    private func humanCan(_ type: MoveType) -> Bool { game.humanLegal.contains { $0.type == type } }

    private func transferMove(_ view: GameView) -> Move? {
        guard let selId = selection.first else { return nil }
        return game.humanLegal.first { $0.type == .pass && $0.cards.contains { $0.identity == selId } }
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        guard let selId = selection.first,
              let card = view.me?.hand?.first(where: { $0.identity == selId }) else { return [] }
        var out: Set<Int> = []
        for (i, b) in view.battles.enumerated() {
            if game.humanLegal.contains(where: { $0.type == .cover && $0.cards == [card] && ($0.attackCards ?? []) == [b.attack] }) {
                out.insert(i)
            }
        }
        return out
    }
}
