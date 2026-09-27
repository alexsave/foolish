// TallyTable.swift - what the table shows and what a touch does (pickemup's
// TableModel, on the TallyKernel seam).
//
// It does not know there is a conversation. It says "stage this" (`onStage`)
// with the kernel's caption, and MessagesViewController does the rest.
//
// WHAT LIVES HERE AND NOWHERE ELSE: the keep marks the player is toggling
// before they press Roll. They are this phone's until the KEEP move carries
// them (T7: the mask is the move), so they are not the kernel's yet; whether a
// tap may toggle at all, and whether those marks may roll, are the kernel's
// verdicts (TrayModel.canKeep, TallyKernel.mayRoll).
//
// THE PREVIEW NUMBERS: what the dice on the tray would score in each open row
// are the kernel's (TallyKernel.preview, tb_api_score_if); Swift scores none.
//
// THE MOTION is the kernel's too: an adopt or a send lays out a plan
// (tb_beats.h), and `refresh(animate:)` hands the newest one to the player.

import Combine
import SwiftUI

@MainActor
public final class TallyTable: ObservableObject {

    public let kernel: TallyKernel
    public var onStage: ((TallyStage) -> Void)?

    @Published public private(set) var view = TallyView()
    /// My keep marks, before Roll sends them.
    @Published public private(set) var marks = Array(repeating: false, count: TrayModel.diceCount)
    public let player = BeatPlayer()
    /// The roll my marks belong to: whose turn, which roll, which dice.
    private var markedRoll: [Int] = []
    /// The newest plan handed to the player (tb_api_beats_serial).
    private var playedSerial = -1

    public init(kernel: TallyKernel) {
        self.kernel = kernel
        refresh(animate: false)
    }

    // MARK: reading

    /// Read everything back from the resident. `animate`: play the plan the
    /// kernel laid out for what just came in or went out, if it is new.
    public func refresh(animate: Bool = true) {
        let new = kernel.view()
        view = new
        // MY MARKS ARE KEPT ACROSS A STAGE AND ITS CANCEL, and reset to the
        // kernel's held dice when a roll I may keep from is new: a staged KEEP's
        // draft is roll n + 1, so comparing with the view before it would
        // forget the marks the moment Messages' X brings roll n back.
        if new.tray.canKeep {
            let roll = [new.tray.turn ?? -1, new.tray.roll, new.tray.dice.reduce(0) { $0 * 7 + $1 }]
            if roll != markedRoll {
                marks = new.tray.kept
                markedRoll = roll
            }
        }
        let serial = Tb.beatsSerial
        guard serial != playedSerial else { return }
        playedSerial = serial
        if animate { player.play(Tb.beatsNow()) } else { player.clear() }
    }

    // MARK: what the screens draw

    /// The tray as it should look: the kernel's, with my marks while I toggle.
    public var tray: TrayModel {
        var t = view.tray
        if t.canKeep { t.kept = marks }
        t.canRoll = t.canRoll && kernel.mayRoll(keeping: TrayModel.mask(t.kept))
        return t
    }

    public var myCard: CardModel? { view.myCard }

    /// The open rows' preview numbers, only while I may score these dice.
    public var preview: [Int?] {
        let t = view.tray
        guard t.canScore, t.allKnown else { return Array(repeating: nil, count: Category.count) }
        let p = kernel.preview()
        return p.count == Category.count ? p : Array(repeating: nil, count: Category.count)
    }

    /// Everybody but me, in seat order (a spectator sees them all).
    public var others: [CardModel] { view.cards.filter { $0.seat != view.me } }

    public func name(_ seat: Int) -> String { seat < view.names.count ? view.names[seat] : "" }

    // MARK: touches, each one kernel call

    public func toggle(_ die: Int) {
        guard view.tray.canKeep, die >= 0, die < marks.count else { return }
        marks[die].toggle()
    }

    public func roll() {
        stage(kernel.roll(keeping: TrayModel.mask(tray.kept)))
    }

    public func score(_ c: Category) {
        stage(kernel.score(c))
    }

    public func join() { stage(kernel.join()) }
    public func leave() { stage(kernel.leave()) }
    public func start() { stage(kernel.start()) }

    private func stage(_ s: TallyStage?) {
        guard let s else { return }
        refresh(animate: false)
        onStage?(s)
    }

    // MARK: the conversation's echoes

    /// Messages' X on the staged bubble.
    public func cancelStaged() {
        kernel.cancelDraft()
        refresh(animate: false)
    }
}
