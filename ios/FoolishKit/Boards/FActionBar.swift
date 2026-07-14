// FActionBar.swift — Pass / Take / Done (§5.4). Compact wooden plaques, not
// full-width slabs: the web's action buttons are small corner controls. EVERY
// enable state is driven by the kernel's legal-move menu passed in — never
// hand-computed (§3, §5.4). Attacks and covers are played by tapping/dragging
// cards (TableView); the bar owns only the zero-card and selection control moves.

import SwiftUI

public struct FActionBar: View {
    public let canPickup: Bool     // MOVE_PICKUP in the menu (defender)
    public let canDone: Bool       // MOVE_GOOD in the menu (finish attacking / бито)
    public let canTransfer: Bool   // a MOVE_PASS using the current selection
    public let onPickup: () -> Void
    public let onDone: () -> Void
    public let onTransfer: () -> Void

    public init(canPickup: Bool, canDone: Bool, canTransfer: Bool,
                onPickup: @escaping () -> Void, onDone: @escaping () -> Void,
                onTransfer: @escaping () -> Void) {
        self.canPickup = canPickup
        self.canDone = canDone
        self.canTransfer = canTransfer
        self.onPickup = onPickup
        self.onDone = onDone
        self.onTransfer = onTransfer
    }

    public var body: some View {
        HStack(spacing: FSpace.s) {
            if canTransfer { plaque(FStrings.t("pass"), seed: 0.2, action: onTransfer) }
            if canPickup { plaque(FStrings.t("pickup"), seed: 0.5, action: onPickup) }
            if canDone { plaque(FStrings.t("good"), seed: 0.85, primary: true, action: onDone) }
        }
        .frame(maxWidth: .infinity)
        .animation(FMotion.chrome, value: canPickup)
        .animation(FMotion.chrome, value: canDone)
        .animation(FMotion.chrome, value: canTransfer)
    }

    private func plaque(_ label: String, seed: Double, primary: Bool = false,
                        action: @escaping () -> Void) -> some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            Text(label)
                .font(FType.title(16))
                .tracking(0.5)
                .foregroundColor(FColor.textPrimary)
                .shadow(color: .black.opacity(0.75), radius: 1, x: 0, y: 1)
                .frame(minWidth: 92, minHeight: 46)
                .padding(.horizontal, FSpace.m)
                .background(WoodSurface(seed: seed, cornerRadius: FRadius.button))
                .overlay(
                    RoundedRectangle(cornerRadius: FRadius.button, style: .continuous)
                        .strokeBorder(primary ? FColor.accent : FColor.woodDark, lineWidth: primary ? 2 : 1)
                )
        }
        .buttonStyle(FPressStyle())
    }
}
