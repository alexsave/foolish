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

    /// Round-5 owner note: "attackers shouldn't have swords except for first
    /// attacker when there are no cards on table" — this bubble view used to
    /// give EVERY non-defender a sword, which is only right once the bout is
    /// open. On an EMPTY table only the seat that may actually open it (the
    /// first attacker) can act at all, so only THAT seat gets the sword; once
    /// the table has a battle, throw-ins make every other non-defender who
    /// hasn't said good a real attacker too. Inlined rather than imported
    /// because `MessageBoardView` can't reach across files for it — this is
    /// the exact twin of `MessageTableView.showsSword` (`MessageTableView.
    /// swift:393`), the live board's version of the same rule; keep both in
    /// sync if the rule ever changes again.
    private func showsSword(seat: Int, isOut: Bool, _ view: GameView) -> Bool {
        guard seat != view.defender, !isOut, !view.hasSaidGood(seat) else { return false }
        return view.battles.isEmpty ? seat == view.firstAttacker : true
    }

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
                               isAttacker: showsSword(seat: p.seat, isOut: p.isOut, view),
                               saidGood: view.hasSaidGood(p.seat),
                               isOut: p.isOut)   // wool bubble → bone text + shadow (like the board)
                        .position(ringPoint(seat: p.seat, n: view.players.count, in: geo.size))
                }

                FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                          hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                // Note 10 (same fix as MessageTableView, this being the same
                // FDeckWell/FDiscardPile pair, just this view's public/spectator
                // rendering of it): -3 puts the discard's own centre on the
                // draw deck's bottom-card centre — see MessageTableView's
                // discard placement for the full derivation.
                discardPile
                    .offset(y: -3)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                if view.isOver {
                    // Round-5 M10 sweep: this was full-opacity bone text with NO
                    // shadow at all, sitting straight on the wool — the finding's
                    // "no fixed-opacity foreground can survive it" applies even
                    // without a reduced-opacity color; the missing half of the
                    // known fix (real shadow) was missing here too. Round-6 #17:
                    // this is plain text on the wool weave (no wood behind it),
                    // so it takes `onWoolText` (Tokens.swift) - thick black ink,
                    // not the bone-on-dark-shadow combo, which is wood's half of
                    // the pairing (MessageTableView's plank uses that one).
                    Text(view.gameOver >= 0
                        ? FStrings.t("ios.msg.isfool", ["name": name(view.gameOver)])
                        : FStrings.t("game_over"))
                        .font(.subheadline)
                        .onWoolText()
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
