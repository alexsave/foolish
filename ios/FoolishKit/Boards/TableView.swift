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
                    VStack(spacing: 0) {
                        opponentsStrip(view)
                            .frame(height: geo.size.height * 0.22)
                        battlesZone(view)
                            .frame(height: geo.size.height * 0.40)
                        Spacer(minLength: 0)
                        actionBar(view)
                            .padding(.bottom, FSpace.s)
                        handZone(view)
                            .frame(height: geo.size.height * 0.26)
                    }
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
        // Flat felt + a subtle vignette — the only gradient allowed (§5.1).
        FColor.table
            .overlay(
                RadialGradient(colors: [.clear, .black.opacity(0.35)],
                               center: .center, startRadius: 40, endRadius: 520)
            )
            .ignoresSafeArea()
    }

    private func opponentsStrip(_ view: GameView) -> some View {
        let opponents = view.players.filter { $0.seat != game.humanSeat }
        return HStack(alignment: .top, spacing: FSpace.m) {
            ForEach(opponents) { p in
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
        }
        .padding(.top, FSpace.m)
        .frame(maxWidth: .infinity)
    }

    private func battlesZone(_ view: GameView) -> some View {
        HStack(alignment: .center, spacing: FSpace.s) {
            FBattleGrid(
                battles: view.battles,
                trumpSuit: view.trumpSuit,
                coverable: coverableBattles(view),
                onTapBattle: { idx in tapBattle(idx, view) }
            )
            .frame(maxWidth: .infinity)

            FDeckWell(
                deckCount: view.deckCount,
                flipped: view.flipped,
                hasFlipped: view.hasFlipped,
                trumpSuit: view.trumpSuit
            )
            .padding(.trailing, FSpace.m)
        }
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
