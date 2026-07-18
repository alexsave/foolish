// FActionBar.swift — the five wooden action buttons, matching the WEB client's
// ActionButtons (Attack / Cover / Pass / Take / Done). EVERY enable state is
// driven by the kernel's legal menu (via CardPlay over the current selection),
// never hand-computed (§3). Attacker sees Attack + Done; defender sees Cover +
// Pass + Take. A card is played by selecting it and tapping a button here, by
// tapping a target battle, or by dragging — this bar is the button path.

import SwiftUI

public struct FActionBar: View {
    public let canAttack: Bool     // attacker: the selection is a legal attack
    public let canCover: Bool      // defender: the selection can cover some uncovered attack
    public let canPass: Bool       // defender: the selection is a legal pass/transfer
    public let canPickup: Bool     // defender: MOVE_PICKUP available (take the table)
    public let canDone: Bool       // attacker: MOVE_GOOD available (бито / finish attacking)
    public let onAttack: () -> Void
    public let onCover: () -> Void
    public let onPass: () -> Void
    public let onPickup: () -> Void
    public let onDone: () -> Void

    public init(canAttack: Bool = false, canCover: Bool = false, canPass: Bool = false,
                canPickup: Bool, canDone: Bool,
                onAttack: @escaping () -> Void = {}, onCover: @escaping () -> Void = {},
                onPass: @escaping () -> Void = {},
                onPickup: @escaping () -> Void, onDone: @escaping () -> Void) {
        self.canAttack = canAttack; self.canCover = canCover; self.canPass = canPass
        self.canPickup = canPickup; self.canDone = canDone
        self.onAttack = onAttack; self.onCover = onCover; self.onPass = onPass
        self.onPickup = onPickup; self.onDone = onDone
    }

    public var body: some View {
        // Small wooden pills, right-aligned (web parity — the action buttons sit on
        // the right, sized to their labels, not full-width). Selection-driven plays
        // first (Attack/Cover/Pass), then the zero-card controls (Take/Good).
        HStack(spacing: FSpace.s) {
            Spacer(minLength: 0)
            if canAttack { FButton(FStrings.t("attack"), kind: .wood, compact: true, action: onAttack) }
            if canCover  { FButton(FStrings.t("cover"),  kind: .wood, compact: true, action: onCover) }
            if canPass   { FButton(FStrings.t("pass"),   kind: .wood, compact: true, action: onPass) }
            if canPickup { FButton(FStrings.t("pickup"), kind: .wood, compact: true, action: onPickup) }
            if canDone   { FButton(FStrings.t("good"),   kind: .wood, compact: true, action: onDone) }
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, FSpace.m)
        .animation(FMotion.chrome, value: canAttack)
        .animation(FMotion.chrome, value: canCover)
        .animation(FMotion.chrome, value: canPass)
        .animation(FMotion.chrome, value: canPickup)
        .animation(FMotion.chrome, value: canDone)
    }
}
