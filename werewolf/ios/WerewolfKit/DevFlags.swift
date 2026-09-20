// House flags. THE WHOLE FILE IS DEBUG-ONLY, and that is the security property,
// not a convenience: nothing in here can exist in the shipped binary, so nothing
// in here can be reached by a player.
//
// Ported from the fork's MessageDevBoard.flag, including the reasoning, because
// the reasoning is the point:
//
//   With no flag file, the debug value IS the shipping value, passed in by the
//   caller.
//
// That is what stops the two builds drifting. The trap it closes is the shape
// every codebase grows on its own: a Release constant over here, and a separate
// DEBUG knob over there whose default is its own hardcoded copy of it. Flip one
// and a debug install silently runs a different product from a release one, and
// every screenshot and every measurement taken on that install is about a
// product nobody will ever ship.
//
// So there is ONE knob per behaviour and the shipping value is its argument.
// Do not invent a second one.
#if DEBUG || SOLO_TESTING
import Foundation

public enum DevFlags {

    /// A flag's value in a debug build: `dev.flags` if it names the key, the
    /// SHIPPING value otherwise.
    ///
    /// `dev.flags` holds `key=0|1` pairs separated by spaces or newlines, in the
    /// App Group:
    ///
    ///   printf 'solo.seatpicker=1' > "$APPGROUP/dev.flags"
    ///
    /// Read ONCE per appex process, so a change takes effect on the next open
    /// rather than mid-night. Mid-night would be worse than useless here: the
    /// night's whole contract is that one seat's screen is one seat's screen.
    public static func flag(_ key: String, shipping: Bool) -> Bool {
        table[key] ?? shipping
    }

    private static let appGroup = "group.cards.werewolf.msg"
    private static let flagsFile = "dev.flags"

    private static let table: [String: Bool] = {
        guard let dir = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(flagsFile),
                                    encoding: .utf8)
        else { return [:] }
        var out: [String: Bool] = [:]
        for pair in raw.split(whereSeparator: { $0 == " " || $0 == "\n" }) {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            switch kv[1] {
            case "1", "true", "on":   out[String(kv[0])] = true
            case "0", "false", "off": out[String(kv[0])] = false
            default: continue
            }
        }
        return out
    }()
}
#endif
