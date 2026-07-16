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

                    deckWell(view)
                        .position(x: geo.size.width * 0.14, y: geo.size.height * 0.14)

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
                    .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: view)
                } else {
                    ProgressView().tint(FColor.textPrimary)
                }
            }
        }
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
        // the subtle vignette baked into WoolBackground.
        WoolBackground()
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
            onTapBattle: { idx in tapBattle(idx, view) }
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
            onTap: { card in tapCard(card, view) }
        )
        .padding(.horizontal, FSpace.s)
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
