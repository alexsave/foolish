// Haptics.swift — the one haptic map (§5.2). Every haptic in the app comes from
// here; no view calls UIFeedbackGenerator directly. Honors the Settings haptics
// toggle (§16.E3) via `isEnabled`.

import UIKit

public enum FHaptic {
    case pickUp     // .light  — card lifted
    case drop       // .medium — legal drop
    case reject     // .rigid  — illegal move / server reject
    case win        // .success notification — round win
}

public enum Haptics {
    /// Flipped by Settings. Default on.
    public static var isEnabled: Bool = true

    public static func fire(_ h: FHaptic) {
        guard isEnabled else { return }
        switch h {
        case .pickUp: UIImpactFeedbackGenerator(style: .light).impactOccurred()
        case .drop:   UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        case .reject: UIImpactFeedbackGenerator(style: .rigid).impactOccurred()
        case .win:    UINotificationFeedbackGenerator().notificationOccurred(.success)
        }
    }
}
