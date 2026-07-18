// FActionBar.swift — the action buttons, matching the WEB ActionButtons: a
// VERTICAL stack of small wooden pills on the RIGHT (Attack / Cover / Pass / Take
// / Good), plus an optional Undo used only by the iMessage board once a move is
// staged. EVERY enable state is driven by the kernel's legal menu (via CardPlay),
// never hand-computed (§3). A card is played by selecting it and tapping a button,
// by tapping a target battle, or by dragging.

import SwiftUI

public struct FActionBar: View {
    public let canAttack: Bool     // attacker: the selection is a legal attack
    public let canCover: Bool      // defender: the selection can cover some uncovered attack
    public let canPass: Bool       // defender: the selection is a legal pass/transfer
    public let canPickup: Bool     // defender: MOVE_PICKUP available (take the table)
    public let canDone: Bool       // attacker: MOVE_GOOD available (бито), all covered
    public let canUndo: Bool       // iMessage: a move is staged — un-stage it
    public let onAttack: () -> Void
    public let onCover: () -> Void
    public let onPass: () -> Void
    public let onPickup: () -> Void
    public let onDone: () -> Void
    public let onUndo: () -> Void

    public init(canAttack: Bool = false, canCover: Bool = false, canPass: Bool = false,
                canPickup: Bool = false, canDone: Bool = false, canUndo: Bool = false,
                onAttack: @escaping () -> Void = {}, onCover: @escaping () -> Void = {},
                onPass: @escaping () -> Void = {}, onPickup: @escaping () -> Void = {},
                onDone: @escaping () -> Void = {}, onUndo: @escaping () -> Void = {}) {
        self.canAttack = canAttack; self.canCover = canCover; self.canPass = canPass
        self.canPickup = canPickup; self.canDone = canDone; self.canUndo = canUndo
        self.onAttack = onAttack; self.onCover = onCover; self.onPass = onPass
        self.onPickup = onPickup; self.onDone = onDone; self.onUndo = onUndo
    }

    public var body: some View {
        // Small wooden pills, stacked vertically, pinned right (web parity).
        HStack {
            Spacer(minLength: 0)
            VStack(alignment: .trailing, spacing: FSpace.s) {
                if canAttack { FButton(FStrings.t("attack"), kind: .wood, compact: true, action: onAttack) }
                if canCover  { FButton(FStrings.t("cover"),  kind: .wood, compact: true, action: onCover) }
                if canPass   { FButton(FStrings.t("pass"),   kind: .wood, compact: true, action: onPass) }
                if canPickup { FButton(FStrings.t("pickup"), kind: .wood, compact: true, action: onPickup) }
                if canDone   { FButton(FStrings.t("good"),   kind: .wood, compact: true, action: onDone) }
                if canUndo   { FButton(FStrings.t("ios.msg.undo"), kind: .wood, compact: true, action: onUndo) }
            }
        }
        .padding(.horizontal, FSpace.m)
        .animation(FMotion.chrome, value: canAttack)
        .animation(FMotion.chrome, value: canCover)
        .animation(FMotion.chrome, value: canPass)
        .animation(FMotion.chrome, value: canPickup)
        .animation(FMotion.chrome, value: canDone)
        .animation(FMotion.chrome, value: canUndo)
    }
}
