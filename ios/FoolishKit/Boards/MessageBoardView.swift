// MessageBoardView — a READ-ONLY render of a decoded GameView, for the iMessage
// extension's expanded bubble (and, later, the 300×195 snapshot). It draws only
// what a GameView carries — every seat is a card back with its count, and a
// spectator view carries no hand at all — so it is PUBLIC-safe by construction
// (the snapshot appears on lock screens). It reuses the app's board grammar
// (FSeatTag is FSeatBadge seen from further away / FBattleGrid / FDeckWell) so
// a message looks like the game, not a second UI.
//
// No interaction, no rules: the board comes from the kernel (fio_msg_decode →
// fio_state_packed → MaskedView), this only lays it out. The interactive turn UI
// (tap-to-attack, cover, Send) is a later milestone on top of the same GameView.
import SwiftUI

public struct MessageBoardView: View {
    /// Re-render on a settings change (see FPrefs). This is the BUBBLE board -
    /// the snapshot Messages renders in the transcript - and its game-over
    /// caption comes from `FStrings.t`, which resolves at body-eval time. Same
    /// class of staleness as `SendHintReminder`'s send caption.
    @ObservedObject private var prefs = FPrefs.shared
    private let view: GameView
    private let names: [Int: String]
    /// Seat tags at stations (`tagsBoard`) or the live board's ring of full
    /// badges (`ringBoard`) - PublicBoardLayout.tagsByDefault. Defaulted to
    /// what THIS build ships or is knobbed to, and a parameter so a test can
    /// draw both.
    private let tags: Bool

    /// `names` maps seat → display name (from the FMSG `joins`); absent seats
    /// fall back to a neutral "Seat N".
    public init(view: GameView, names: [Int: String] = [:], tags: Bool? = nil) {
        self.view = view
        self.names = names
        // nil, not `PublicBoardLayout.tagsOn`, as the default: a public init
        // cannot name an internal symbol in a default argument.
        self.tags = tags ?? PublicBoardLayout.tagsOn
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

    // WHY A CHECK IS RARE IN THE BUBBLE, and why that is not a bug.
    //
    // Round 47, owner: "why do only swords and shields show up in the bubble
    // preview. We should see checkboxes as well."
    //
    // Nothing here is missing. `goodMask` survives the wire into the view
    // (MaskedView), this passes it as `saidGood`, and `FSeatBadge.mark` returns
    // `.check` for it ahead of every other mark. What is rare is the STATE.
    //
    // A bubble is baked from the board AFTER the move it carries, and
    // `good_players_mask` is cleared by two of the moves that can be in one:
    // every attack resets it (game.c:760, a new card on the table reopens the
    // question) and the end of a bout clears it with the table (game.c:863).
    // Saying good is the only thing that SETS it. So in a two-player game the
    // lone attacker's good always ends the bout and clears the mask on the same
    // move, and the picture is a table where nobody has said good - correctly.
    //
    // It shows from three seats up, where one attacker can say good while
    // another has not: the bout does not end, the mask stands, and that seat
    // wears its check in the bubble. Filmed on the live board at eight seats.
    //
    // So a bubble that shows a check for a two-player good would have to depict
    // the state BEFORE the settlement it is announcing, which is a different
    // picture from the one every other bubble draws.

    public var body: some View {
        // Same grammar as the live board (MessageTableView): deck pinned top-left,
        // discard top-right, seats round the edge, battles dead-centre - just no
        // self hand (this is the PUBLIC spectator snapshot). Absolute placement
        // so the corners never push the centred pieces.
        //
        // NOT the live board's ring, though. This board is a THUMBNAIL - the
        // 300x195 bubble is its whole reason to exist - and at eight seats the
        // live board's full badges on a 35% ellipse did not fit it: the top
        // seat's name was clipped off the picture, the fans lay over the
        // battle cards, and the marks piled up along the bottom edge. Every
        // position and every size below comes from PublicBoardLayout, which
        // places compact seat tags (FSeatTag) at fixed stations round the
        // edge and shrinks the corners and the battles only as far as those
        // tags require - so a two-player bubble is drawn exactly as it was,
        // and an eight-player one is drawn at all.
        GeometryReader { geo in
            if tags {
                tagsBoard(geo.size)
            } else {
                ringBoard(geo.size)
            }
        }
        // Round-7 #4: the 8pt inset lives OUTSIDE the GeometryReader, not on the
        // ZStack inside it. With it inside, `geo.size` was the FULL bubble while
        // the ZStack's content was 16pt narrower, so `ringPoint(in: geo.size)`
        // placed the seat badges against the full width (centre at width/2) while
        // FBattleGrid centred inside the padded width (centre at (width-16)/2) -
        // the badges landed 8pt right of the battle cluster, which is the bubble
        // card "not centered" report. Padding the GeometryReader instead makes
        // `geo.size` the padded size, so the ring and the centred battles share
        // one coordinate space and line up. (MessageTableView never had this: it
        // centres everything in one unpadded `geo.size`.)
        .padding(8)
    }

    /// THE TAGS: PublicBoardLayout's stations, with the corners and the
    /// battles at the size the tags leave room for. Every piece is SIZED to
    /// its scale rather than transformed, so a card on this board carries
    /// FCard's 1pt edge exactly as a card on the live board does.
    private func tagsBoard(_ size: CGSize) -> some View {
        let n = view.players.count
        let corner = PublicBoardLayout.cornerScale(n: n, in: size)
        let grid = PublicBoardLayout.gridScale(n: n, pairs: view.battles.count, in: size)
        return ZStack {
            if !view.battles.isEmpty {
                FBattleGrid(battles: view.battles, trumpSuit: view.trumpSuit, scale: grid)
            }

            FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                      hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit, scale: corner)
                // A smaller well is the same well seen from further away -
                // its bottom card and flipped trump keep the top-left origins
                // FDeckWell pins (round 4 note 6), scaled, not shifted...
                // ...except that it slides out from under the balloon's app
                // icon as it shrinks (PublicBoardLayout.deckSlide): Messages
                // draws that roundel over this corner, and a small well's
                // count sat exactly under it.
                .offset(x: PublicBoardLayout.deckSlide(scale: corner).x,
                        y: PublicBoardLayout.deckSlide(scale: corner).y)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            // Note 10 (same fix as MessageTableView, this being the same
            // FDeckWell/FDiscardPile pair, just this view's public/spectator
            // rendering of it): -3 puts the discard's own centre on the
            // draw deck's bottom-card centre — see MessageTableView's
            // discard placement for the full derivation.
            FDiscardPile(count: view.discardCount, scale: corner)
                .offset(y: PublicBoardLayout.discardLift)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                // Every seat at its station (no self to omit in the public
                // view; seat 0 sits bottom-centre, the rest clockwise). LAST in
                // the stack: at the scale floors a dense table can still run
                // a card corner under a tag, and the name, the count and the
                // mark are the facts that must stay on top.
            ForEach(view.players) { p in
                FSeatTag(name: name(p.seat),
                         handCount: p.handCount,
                         mark: RoleMarkKind.worn(
                            saidGood: view.hasSaidGood(p.seat),
                            isDefender: p.seat == view.defender,
                            isAttacker: showsSword(seat: p.seat, isOut: p.isOut, view),
                            // Round 20's tinted lead sword, exactly as the
                            // live board wears it (MessageTableView's
                            // badge): the seat that opens the bout.
                            opensBout: p.seat == view.firstAttacker),
                         isOut: p.isOut)
                    .position(PublicBoardLayout.seatPoint(seat: p.seat, n: n, in: size))
            }

            if view.isOver {
                // CENTRED, where the battles were: a finished game has an
                // empty table and a seat tag at the bottom edge, and this
                // line used to sit on that seat's badge. Ink as `ringBoard`'s.
                gameOverLine
            }
        }
    }

    /// THE RING, as this board drew before the tags (PublicBoardLayout
    /// .tagsByDefault, `tags=0`): every seat a full FSeatBadge on the 35%
    /// ellipse, the corners and the battles at the live board's size. Kept
    /// whole and unchanged so the knob selects the old picture, not an
    /// approximation of it.
    private func ringBoard(_ size: CGSize) -> some View {
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
                    .position(PublicBoardLayout.ringPoint(seat: p.seat, n: view.players.count, in: size))
            }

            FDeckWell(deckCount: view.deckCount, flipped: view.flipped,
                      hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            FDiscardPile(count: view.discardCount)
                .offset(y: PublicBoardLayout.discardLift)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

            if view.isOver {
                gameOverLine
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .padding(.bottom, 6)
            }
        }
    }

    /// Round-5 M10 sweep: this was full-opacity bone text with NO shadow at
    /// all, sitting straight on the wool — the finding's "no fixed-opacity
    /// foreground can survive it" applies even without a reduced-opacity
    /// color; the missing half of the known fix (real shadow) was missing here
    /// too. Round-6 #17: this is plain text on the wool weave (no wood behind
    /// it), so it takes `onTableText` (Tokens.swift) - thick black ink, not
    /// the bone-on-dark-shadow combo, which is wood's half of the pairing
    /// (MessageTableView's plank uses that one).
    private var gameOverLine: some View {
        Text(view.gameOver >= 0
            ? FStrings.t("ios.msg.isfool", ["name": name(view.gameOver)])
            : FStrings.t("game_over"))
            .font(.subheadline)
            .onTableText()
    }
}
