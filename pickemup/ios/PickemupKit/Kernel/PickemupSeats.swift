// PickemupSeats.swift - this device's seat records and its nickname, kept
// for the kernel (uttt/ios/UtttKit/UtttSeats.swift's shape).
//
// THE RECORDS: the kernel's fixed-layout bytes (PK_API_REC_BYTES, the newest
// 256 games), carried to and from storage and never read here. Messages'
// participant id is a random UUID deleted with the extension, so the kernel
// asks this device's own record of a game first (pk_msg.h, D42).
//
// THE NICKNAME: typed once, this device's name on every roster it joins
// (foolish's App Group nickname, `fmsg.nickname`). Only the extension reads
// it, and Release has no App Group (project.yml), so it lives in the
// extension's own defaults like the records. Raw bytes and a string: no JSON.

import Foundation

public enum PickemupSeats {
    private static let recordsKey = "pickemup.seats.v1"
    private static let nicknameKey = "pickemup.nickname"
    private static var loaded = false

    private static var store: UserDefaults { .standard }

    /// Hand the kernel this device's records, once per process.
    public static func load() {
        guard !loaded else { return }
        Pk.loadSeats(store.data(forKey: recordsKey))
        loaded = true
    }

    /// Store what the kernel recorded since the last flush, if anything.
    public static func flush() {
        guard loaded, let d = Pk.seatsIfDirty() else { return }
        store.set(d, forKey: recordsKey)
    }

    /// The name this device sits down under, "" until one is typed.
    public static var nickname: String {
        get { store.string(forKey: nicknameKey) ?? "" }
        set {
            store.set(newValue, forKey: nicknameKey)
            Pk.nickname(newValue)
        }
    }
}
