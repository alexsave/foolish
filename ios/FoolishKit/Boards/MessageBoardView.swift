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
import Foundation   // sin/cos for the ring placement

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
        // Same grammar as the live board (MessageTableView): deck pinned top-left,
        // discard top-right, seats ringed on a 35% ellipse, battles dead-centre -
        // just no self hand (this is the PUBLIC spectator snapshot). Absolute
        // placement so the corners never push the centred pieces.
        GeometryReader { geo in
            ZStack {
                if !view.battles.isEmpty {
                    FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit)
                }

                // Every seat ringed (no self to omit in the public view; seat is
                // the visual index, so seat 0 sits bottom-centre).
                ForEach(view.players) { p in
                    FSeatBadge(name: name(p.seat),
                               handCount: p.handCount,
                               isDefender: p.seat == view.defender,
                               isAttacker: p.seat != view.defender && !p.isOut && attackersActive,
                               saidGood: view.hasSaidGood(p.seat),
                               isOut: p.isOut,
                               onLight: true)   // beige bubble → dark name text
                        .position(ringPoint(seat: p.seat, n: view.players.count, in: geo.size))
                }

                FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                          hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                discardPile
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                if view.isOver {
                    Text(view.gameOver >= 0
                        ? FStrings.t("ios.msg.isfool", ["name": name(view.gameOver)])
                        : FStrings.t("game_over"))
                        .font(.subheadline.weight(.semibold)).foregroundStyle(FColor.textPrimary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                        .padding(.bottom, 6)
                }
            }
            .padding(8)
        }
    }

    /// Seat centre on the 35% ellipse (web PlayerRing). Public snapshot has no
    /// "self", so the seat index is the visual index (seat 0 at bottom-centre).
    private func ringPoint(seat: Int, n: Int, in size: CGSize) -> CGPoint {
        let rad = 2 * Double.pi * Double(seat) / Double(max(n, 1))
        let x = (-sin(rad) * 0.35 + 0.5) * size.width
        let y = ( cos(rad) * 0.35 + 0.5) * size.height
        return CGPoint(x: x, y: y)
    }

    private var discardPile: some View {
        FDiscardPile(count: view.discardCount)
    }
}
