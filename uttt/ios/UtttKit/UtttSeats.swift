// UtttSeats.swift - this device's seat records, kept for the kernel.
//
// WHY (1.0(9), 2026-09-25): Messages' participant id is a random UUID it
// deletes when the extension is uninstalled or swapped between TestFlight and
// a development build, so a seat proven only by a tag of that id stranded its
// owner as a spectator. The kernel now asks this device's own RECORD of the
// game first (uttt_msg.h, utm_resolve); this file only carries the kernel's
// bytes to and from storage. It never reads them: the layout, the keys and
// the bound (the newest 256 games) are the kernel's.
//
// THE EXTENSION'S OWN DEFAULTS, not an App Group: the extension is the only
// reader, and Release has no App Group. Raw fixed-layout bytes, not JSON.
// They live as long as the extension's container: an update or a re-sign
// keeps them; deleting the app does not, and then the sender witness is left.

import Foundation

public enum UtttSeats {
    private static let base = "uttt.seats.v1"

    /// Whose records the kernel holds: "" is this device. A DEBUG dev seat
    /// ("a", "b") is a phone of its own, so the rig's two players on one
    /// simulator do not read each other's seats.
    private static var loaded: String?

    /// Hand the kernel `persona`'s records, saving the ones it held first.
    public static func use(_ persona: String) {
        if loaded == persona { return }
        flush()
        Uttt.loadSeats(UserDefaults.standard.data(forKey: key(persona)) ?? Data())
        loaded = persona
    }

    /// Store what the kernel recorded since the last flush, if anything.
    public static func flush() {
        guard let p = loaded, Uttt.seatsDirty, let d = Uttt.saveSeats() else { return }
        UserDefaults.standard.set(d, forKey: key(p))
    }

    private static func key(_ p: String) -> String { p.isEmpty ? base : "\(base).\(p)" }
}
