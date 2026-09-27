// Phones.swift - one kernel, two phones, for the tests: each person's
// identity, nickname and seat records are their own device's, so switching
// person swaps them (pickemup/ios/PickemupKitTests/Phones.swift's shape).

import Foundation
@testable import TallybonesKit

@MainActor
enum Phones {
    static let nicks = ["Alex", "Bo"]
    private static var records: [Int: Data] = [:]
    private static var current: Int?

    /// Hold person `i`'s phone.
    static func be(_ i: Int) {
        if let c = current, let d = Tb.seatsIfDirty() { records[c] = d }
        Tb.loadSeats(records[i])
        Tb.me(Data(repeating: UInt8(i * 37 + 1), count: 16))
        Tb.nickname(nicks[i])
        Tb.sender(of: nil)
        current = i
    }

    static func seed(_ k: Int) -> [UInt8] { (0..<32).map { UInt8(($0 * 29 + k * 7 + 5) & 0xFF) } }

    /// A DM: Alex invites, Bo joins and starts in one bubble, and Alex opens
    /// it: Alex's phone is in hand, seat 0, on roll 1 of the first turn (T12).
    /// The start bubble's link, or nil.
    @discardableResult
    static func dmStarted(seed k: Int = 1) -> String? {
        records = [:]
        current = nil
        be(0)
        guard Tb.newGame(dm: true, seed: seed(k)), let invite = Tb.text else { return nil }
        be(1)
        Tb.sender(of: invite, isDM: true, iSent: false)
        guard Tb.read(invite) == 0, Tb.joinStart() == 1, let start = Tb.text else { return nil }
        be(0)
        Tb.sender(of: start, isDM: true, iSent: false)
        guard Tb.adopt(start, arrival: false) == 0 else { return nil }
        return start
    }
}
