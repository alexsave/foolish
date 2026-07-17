// MessageBoardView — a READ-ONLY render of a decoded GameView, for the iMessage
// extension's expanded bubble (and, later, the 300×195 snapshot). It draws only
// what a GameView carries — opponents are card-backs by count, the viewer's own
// hand shows if present — so it is PUBLIC-safe by construction (the snapshot
// appears on lock screens). It reuses the app's board grammar (FSeatBadge /
// FBattleGrid / FDeckWell) so a message looks like the game, not a second UI.
//
// No interaction, no rules: the board comes from the kernel (fio_msg_decode →
// fio_state_packed → MaskedView), this only lays it out. The interactive turn UI
// (tap-to-attack, cover, Send) is a later milestone on top of the same GameView.
import SwiftUI

public struct MessageBoardView: View {
    private let view: GameView
    private let names: [Int: String]

    /// `names` maps seat → display name (from the FMSG `joins`); absent seats
    /// fall back to a neutral "Seat N".
    public init(view: GameView, names: [Int: String] = [:]) {
        self.view = view
        self.names = names
    }

    private func name(_ seat: Int) -> String { names[seat] ?? "Seat \(seat + 1)" }

    private var attackersActive: Bool { view.battles.contains { $0.defense == nil } || view.battles.isEmpty }

    public var body: some View {
        VStack(spacing: 14) {
            // Seats — every player as a mini badge (count-only hands).
            HStack(spacing: 10) {
                ForEach(view.players) { p in
                    FSeatBadge(name: name(p.seat),
                               handCount: p.handCount,
                               isDefender: p.seat == view.defender,
                               isAttacker: p.seat != view.defender && !p.isOut && attackersActive,
                               isOut: p.isOut)
                }
            }

            // Center: deck + trump, the battles, the discard count.
            HStack(alignment: .center, spacing: 18) {
                FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                          hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)

                if view.battles.isEmpty {
                    Text(FStrings.t("ios.nobattle")).font(.caption).foregroundStyle(.secondary)
                } else {
                    FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit)
                }

                discardPile
            }

            if view.isOver {
                Text(view.gameOver >= 0
                    ? FStrings.t("ios.msg.isfool", ["name": name(view.gameOver)])
                    : FStrings.t("game_over"))
                    .font(.subheadline.weight(.semibold))
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var discardPile: some View {
        VStack(spacing: 3) {
            Image(systemName: "rectangle.stack.fill").font(.title3).foregroundStyle(.secondary)
            Text("\(view.discardCount)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
        }
    }
}
