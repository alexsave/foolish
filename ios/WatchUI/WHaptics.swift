// WHaptics.swift — the watchOS haptic vocabulary (docs/WATCHOS_G_SPEC.md §8). Every
// buzz means "you can act now"; everything else is silent. Mapped to WKInterfaceDevice.

import WatchKit

enum WHaptic {
    case detent     // .click        — one Crown detent
    case liveOrTurn // .notification — a rank you hold went live / you became defender
    case confirmed  // .success      — your move confirmed
    case rejected   // .failure      — your move rejected (paired with RejectGlow)
    case tookOrOver // .stop         — you picked up / game over
}

enum WHaptics {
    static var isEnabled = true

    static func fire(_ h: WHaptic) {
        guard isEnabled else { return }
        let d = WKInterfaceDevice.current()
        switch h {
        case .detent:     d.play(.click)
        case .liveOrTurn: d.play(.notification)
        case .confirmed:  d.play(.success)
        case .rejected:   d.play(.failure)
        case .tookOrOver: d.play(.stop)
        }
    }
}
