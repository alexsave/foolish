// Phones.swift - one kernel, several phones, for the tests: each person's
// identity, nickname and seat records are their own device's, so switching
// person swaps them (pickemup/c/ios/pk_api_smoke.c's `be`, from Swift).

import Foundation
@testable import PickemupKit

enum Phones {
    static let nicks = ["Alex", "Bo", "Cy", "Dee"]
    private static var records: [Int: Data] = [:]
    private static var current: Int?

    /// Hold person `i`'s phone.
    static func be(_ i: Int) {
        if let c = current, let d = Pk.seatsIfDirty() { records[c] = d }
        Pk.loadSeats(records[i])
        Pk.me(Data(repeating: UInt8(i * 37 + 1), count: 16))
        Pk.nickname(nicks[i])
        Pk.sender(of: nil)
        current = i
    }

    static func reset() {
        records = [:]
        current = nil
    }

    static func seed(_ k: Int) -> [UInt8] { (0..<32).map { UInt8(($0 * 29 + k * 7 + 5) & 0xFF) } }

    /// A DM: Alex invites, Bo joins and starts in one bubble (4.6.5), so Bo
    /// is resident in seat 1 and moves first (D28).
    @discardableResult
    static func dmStartedByBo(seed k: Int = 1) -> String? {
        reset()
        be(0)
        guard Pk.newGame(dm: true, seed: seed(k)), let invite = Pk.text else { return nil }
        be(1)
        Pk.sender(of: invite, isDM: true, iSent: false)
        guard Pk.read(invite) == 0, Pk.joinStart() == 1 else { return nil }
        return invite
    }

    /// The first DM deal whose starter holds a wild that is not their last card.
    static func dmWithWild() -> Int? {
        for k in 0..<400 {
            guard dmStartedByBo(seed: k) != nil, let v = Pk.view() else { continue }
            if let pos = v.myHand.indices.first(where: { Pk.isWild($0) }) { return pos }
        }
        return nil
    }

    /// A hand position that plays and is not a wild, if Bo has one.
    static func plainPlayable() -> Int? {
        guard let v = Pk.view() else { return nil }
        return v.myHand.indices.first { v.myPlayable[$0] != 0 && !Pk.isWild($0) }
    }
}
