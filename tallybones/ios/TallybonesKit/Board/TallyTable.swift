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
// come from `previewScores`, a closure, because scoring is the kernel's and
// Swift computes none of it. KERNEL: set it to the bridge's per-category
// preview (tb_api_preview or whatever the kernel names it) in
// TallybonesHost.init; the stand-in's is StandInKernel.preview.

import Combine
import SwiftUI

@MainActor
public final class TallyTable: ObservableObject {

    public let kernel: TallyKernel
    /// Dice -> what they would score in each category (Category order); nil
    /// for a row the kernel gives no number for.
    public var previewScores: ([Int]) -> [Int?]
    public var onStage: ((TallyStage) -> Void)?

    @Published public private(set) var view = TallyView()
    /// My keep marks, before Roll sends them.
    @Published public private(set) var marks = Array(repeating: false, count: TrayModel.diceCount)
    public let tumble = TumblePlayer()

    public init(kernel: TallyKernel, previewScores: @escaping ([Int]) -> [Int?]) {
        self.kernel = kernel
        self.previewScores = previewScores
        refresh(animate: false)
    }

    // MARK: reading

    /// Read everything back from the resident. `animate`: dice that were
    /// blank (or a new roll's) and now have values tumble in.
    public func refresh(animate: Bool = true) {
        let old = view
        let new = kernel.view()
        view = new
        let newRoll = new.tray.roll != old.tray.roll || new.tray.turn != old.tray.turn
        if newRoll || !new.tray.canKeep {
            marks = new.tray.kept
        }
        guard animate else { return }
        // KERNEL: the kernel's plan names which dice settle (T9); until then
        // the dice that just got a value, or all five on a new turn's roll 1.
        var rolled = Set<Int>()
        for i in 0..<TrayModel.diceCount where i < new.tray.dice.count && new.tray.dice[i] != 0 {
            let was = i < old.tray.dice.count ? old.tray.dice[i] : 0
            let turnBegan = new.tray.turn != old.tray.turn && new.tray.roll == 1
            if was == 0 || turnBegan || (newRoll && !new.tray.kept[i]) { rolled.insert(i) }
        }
        if !rolled.isEmpty { tumble.tumble(rolled) }
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
        let p = previewScores(t.dice)
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
