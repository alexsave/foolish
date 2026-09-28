// THE ACTION PILLS, THE UNDO PILL AND THE SETTINGS SQUARES - the chrome that
// floats over the hand.
//
// Every enable state here is the kernel's legal menu narrowed by
// `BoardActionMenu.resolve`, never a hand-rolled "is it my turn". What this
// file owns is the two things that are about this SCREEN rather than about
// Durak: the gates (`actionGates`) and the slots the pills merely appear
// inside, which is what keeps a pill from being animated into place.

import SwiftUI
import Foundation

extension MessageTableView {

    /// The Settings + Help squares, mirroring `actionBar` on the LEFT (1.0(4)).
    /// Now the shared `SettingsHelpSquares` pair — the New-game setup and the
    /// lobby float the same component at the same corner (durak-rules-redesign),
    /// so the three screens cannot drift apart. Persistent (unlike the move
    /// buttons) — Settings and Help are always available while playing.
    var settingsHelpBar: some View {
        SettingsHelpSquares(onSettings: { showSettings = true },
                            onHelp: { showRules = true })
    }

    /// WHAT THE BOARD KNOWS ABOUT ITSELF, as `BoardActionMenu` wants it - the
    /// gates that are not the kernel's to answer because they are about this
    /// screen rather than about Durak. Which pills they turn a kernel probe
    /// into is `BoardActionMenu.resolve`, which is not a view and has a test.
    ///
    /// `boardStill` reads statics nothing publishes (`ActionPillSlot`/
    /// `UndoGate`), which is also why the column that draws this is redrawn on
    /// a short timer rather than on a change.
    private func actionGates(_ view: GameView, selectionIsEmpty: Bool) -> BoardActionMenu.Gates {
        let boardStill = !ActionPillSlot.waitsForStill
            || UndoGate.acceptsNow(cardsVeiled: !animator.hidden.isEmpty)
        return .init(iCanAct: controller.iCanAct,
                     canSend: controller.canSend,
                     playInFlight: playInFlight,
                     boardStill: boardStill,
                     superseded: controller.superseded,
                     pickupHeld: controller.pickupHold != 0,
                     isDefender: view.defender == controller.mySeat,
                     isOut: view.me?.isOut ?? false,
                     tableIsEmpty: view.battles.isEmpty,
                     selectionIsEmpty: selectionIsEmpty)
    }

    func actionBar(_ view: GameView) -> some View {
        let cards = selectedCards(view)
        // ONE kernel answer for every enable-state below, so no two of them can
        // describe different menus - and ONE place that turns it into pills, so
        // the rule can be read and tested without a board on screen. Every
        // "why is this button not there" answer lives in BoardActionMenu now,
        // Take's documented exception to the kernel menu included.
        let menu = BoardActionMenu.resolve(probe(view, cards, .table),
                                           actionGates(view, selectionIsEmpty: cards.isEmpty))
        return FActionBar(
            canAttack: menu.canAttack,
            canCover: menu.canCover,
            canPass: menu.canPass,
            canPickup: menu.canPickup,
            canDone: menu.canDone,
            canUndo: false,   // the board draws its own - see `undoSlot`
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onUndo: { undoAction() }
        )
    }

    /// The Undo pill, built and placed EXACTLY like `settingsHelpBar`: a
    /// constant, always-present, fixed-size slot that the button merely appears
    /// INSIDE of.
    ///
    /// Round-10g (owner: "literally just take whatever you do to the settings
    /// button and do it to the undo button"). Undo used to live in FActionBar's
    /// VStack, whose height and membership change the instant a move is staged
    /// (Attack disappears, Undo appears) - so the pill was INSERTED into a
    /// column that was itself resizing, and SwiftUI animated its position from
    /// wherever the old layout put it. Filmed repeatedly: Undo appearing ~280pt
    /// above its slot and flying down over ~7 frames while the settings squares
    /// - which are always there, at a fixed size - never moved a pixel.
    /// Neither `.animation(nil, value:)`, nor `.transaction { animation = nil }`
    /// on the placement, nor a fixed box around the whole column stopped it;
    /// not inserting anything does. Undo and the play buttons are mutually
    /// exclusive by construction (every play button is gated on `!canSend`,
    /// Undo on `canSend`), so hoisting it out of the column changes no layout.
    var undoSlot: some View {
        ZStack {
            // Not built at all until something is staged. The TimelineView
            // below ticks at 10Hz for as long as it exists, and there is
            // nothing for it to re-ask while there is no move to take back -
            // `undoPill` is handed the same `canSend` inside, which is where
            // the answer itself lives.
            if controller.canSend {
                // …AND NOT WHILE THE BOARD MOVES (UndoGate). The gate reads
                // statics nothing publishes, so the pill is redrawn on a short
                // timer while it is up, and `undoPillTapped` asks again at the
                // tap in case one lands between two redraws.
                TimelineView(.periodic(from: .now, by: 0.1)) { _ in
                    // …and not while my play is still being staged: a Good flies
                    // nothing, and the move is sealed before the collapse begins.
                    // Shown enabled, or not shown - never dimmed, which is what
                    // `UndoGate.hides` buys and what the three-case answer keeps
                    // from being a convention about a pair of Bools.
                    let pill = BoardActionMenu.undoPill(
                        canSend: controller.canSend,
                        retracting: controller.conflictRetracting,
                        still: UndoGate.acceptsNow(cardsVeiled: !animator.hidden.isEmpty) && !playInFlight,
                        hides: UndoGate.hides)
                    if pill != .absent {
                        undoPill(enabled: pill == .enabled)
                    }
                }
            }
        }
        .frame(width: FActionBar.pillWidth, height: 40)
    }

    private func undoPill(enabled: Bool) -> some View {
        // NOT WHILE A RETRACTION IS IN FLIGHT (the audit's U8). Every
        // other door into the controller asks first - `cancelStage` and
        // `apply` both check RETRACTING before anything else - and this
        // one did not, so a tap during the conflict peek ran `undo`,
        // found nothing to take back, and then RE-STAGED the very chain
        // being retracted. Disabled rather than guarded inside the
        // action, on the owner's call: "let's disable the undo button
        // during that then." A control that cannot be pressed has no
        // door to forget.
        FButton(FStrings.t("ios.msg.undo"), kind: .wood,
                enabled: enabled, compact: true,
                fixedWidth: FActionBar.pillWidth, action: undoPillTapped)
    }

    /// The Undo pill's tap: refused while the board moves (UndoGate), then the
    /// same `undoAction` the bubble's X runs - which does NOT ask the gate.
    private func undoPillTapped() {
        guard UndoGate.acceptsNow(cardsVeiled: !animator.hidden.isEmpty), !playInFlight else {
            AnimLog.say("undo refused: the board is still moving")
            return
        }
        undoAction()
    }
}
