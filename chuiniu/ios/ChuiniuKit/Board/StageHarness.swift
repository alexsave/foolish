// StageHarness.swift - DEBUG ONLY: a table of any size inside the real extension,
// for looking at the stage on a simulator (FoolishHarness's idea).
//
// Messages on a simulator has one DM to play in, so a DM lobby (two seats) is
// all the real flow reaches. With `dev.harness` in the App Group (`3`, `6`,
// `6 call`, ...) the drawer instead plays a group game of that many seats into
// the kernel, as the tests' phones do (each its own throwaway defaults and
// identity), and opens on seat 0's table; `call` plays one raise and a call
// so the reveal is on screen. Nothing here exists in a Release build, and the
// links it makes are never staged on purpose (a stage already waiting may
// still pick the harness game up; it is a Debug build's toy).

#if DEBUG
import Foundation

@MainActor
enum Harness {
    static var ran = false

    /// The file's words, or nil (no harness).
    static var spec: (seats: Int, call: Bool)? {
        guard let s = DevFlags(group: ChuiniuDev.group).string("dev.harness") else { return nil }
        let words = s.split(whereSeparator: \.isWhitespace)
        guard let n = words.first.flatMap({ Int($0) }), (2...6).contains(n) else { return nil }
        return (n, words.contains("call"))
    }

    private static let names = ["Alex", "Bo", "Cy", "Di", "Ed", "Fay"]

    private static func phone(_ name: String) -> BridgeKernel {
        let suite = "chuiniu.harness.\(name)"
        let d = UserDefaults(suiteName: suite)!
        d.removePersistentDomain(forName: suite)
        let k = BridgeKernel(store: d, devPerson: false)
        k.me(Data(repeating: UInt8(name.utf8.first!), count: 16))
        k.nickname(name)
        k.sender(nil, isDM: false, iSent: false)
        return k
    }

    /// Play the game into the kernel; the resident is then seat 0's table
    /// (or the reveal). Whether it worked.
    static func play(_ spec: (seats: Int, call: Bool)) -> Bool {
        let seed: [UInt8] = (0..<32).map { UInt8(($0 * 29 + spec.seats) & 0xFF) }
        let alex = phone("Alex")
        guard alex.newGame(dm: false, seed: seed), var link = alex.stagedURL() else { return false }
        for s in 1..<spec.seats {
            let p = phone(names[s])
            guard p.adoptBubble(link) == 0, p.join(name: names[s]), let l = p.stagedURL() else { return false }
            link = l
        }
        if spec.seats < 6 {
            let a = phone("Alex")
            guard a.adoptBubble(link) == 0, a.start(), let l = a.stagedURL() else { return false }
            link = l
        }
        var me = phone("Alex")
        guard me.adoptBubble(link) == 0 else { return false }
        if spec.call {
            guard let m = me.table.menu, me.raise(quantity: m.minimumRaise.quantity + 1, face: 3),
                  let raise = me.stagedURL() else { return false }
            me.sent(raise)
            let bo = phone("Bo")
            guard bo.adoptBubble(raise) == 0, bo.call(), let call = bo.stagedURL() else { return false }
            bo.sent(call)
            me = phone("Alex")
            guard me.adoptBubble(call) == 0 else { return false }
        }
        return true
    }
}
#endif
