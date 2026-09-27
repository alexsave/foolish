// COPIED from pickemup/ios/PickemupKit/Kernel/PickemupSeats.swift at 8e216923 - replaced by the seats lift
//
// This device's seat records and its nickname, kept for the kernel.
//
// THE RECORDS: the kernel's fixed-layout bytes, carried to and from storage
// and never read here (pickemup D42: Messages' participant id is a random
// UUID deleted with the extension, so the kernel asks this device's own
// record of a game first). The stand-in keeps none.
//
// THE NICKNAME: typed once, this device's name on every roster it joins. Only
// the extension reads it, and Release has no App Group (project.yml), so it
// lives in the extension's own defaults. Raw bytes and a string: no JSON.

import Foundation

@MainActor
public enum TallybonesSeats {
    private static let recordsKey = "tallybones.seats.v1"
    private static let nicknameKey = "tallybones.nickname"
    private static var loaded = false

    private static var store: UserDefaults { .standard }

    /// Hand the kernel this device's records, once per process.
    public static func load(into kernel: TallyKernel) {
        guard !loaded else { return }
        kernel.loadSeats(store.data(forKey: recordsKey))
        kernel.setNickname(nickname)
        loaded = true
    }

    /// Store what the kernel recorded since the last flush, if anything.
    public static func flush(_ kernel: TallyKernel) {
        guard loaded, let d = kernel.seatsIfDirty() else { return }
        store.set(d, forKey: recordsKey)
    }

    /// The name this device sits down under, "" until one is typed.
    public static var nickname: String { store.string(forKey: nicknameKey) ?? "" }

    public static func set(nickname: String, kernel: TallyKernel) {
        store.set(nickname, forKey: nicknameKey)
        kernel.setNickname(nickname)
    }
}
