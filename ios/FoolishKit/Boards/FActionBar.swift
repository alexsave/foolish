// FActionBar.swift — Pass/Pickup/Done (§5.4). EVERY enable state is driven by
// the kernel's legal-move menu passed in — never hand-computed (§3, §5.4). The
// bar owns only the zero-card and selection-based control moves; attacks and
// covers are played by tapping cards (TableView).

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
        HStack(spacing: FSpace.m) {
            // Wooden controls, matching the website's physical buttons (§5.4,
            // IOS_PHONE_LAYOUT §3 "Wooden 72×40 buttons").
            if canTransfer {
                FButton(FStrings.t("pass"), kind: .wood, action: onTransfer)
            }
            if canPickup {
                FButton(FStrings.t("pickup"), kind: .wood, action: onPickup)
            }
            if canDone {
                FButton(FStrings.t("good"), kind: .wood, action: onDone)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, FSpace.l)
        .animation(FMotion.chrome, value: canPickup)
        .animation(FMotion.chrome, value: canDone)
        .animation(FMotion.chrome, value: canTransfer)
    }
}
