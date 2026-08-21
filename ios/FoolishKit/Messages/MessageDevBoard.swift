// MessageDevBoard.swift — DEBUG-only seeded board state for verification runs.
//
// NEVER COMPILED INTO A SHIPPING BUILD. The whole file is inside
// `#if DEBUG || SOLO_TESTING`, the same gate MessageDebugFlags uses: a
// TestFlight/App Store build defines neither condition, so none of this exists
// in the product.
//
// WHY IT EXISTS. Some board animations only misbehave at states that are tedious
// to reach by hand. The round-12 case is the pickup sweep: the owner reported
// that "pickup animation sometimes quickly rearranges into grid before moving to
// hand for many players", and reproducing it needs a table carrying ten cards
// with several COVERED pairs among them - minutes of careful tapping per
// attempt, and not reliably reachable at all.
//
// SO THE STATE IS A CONSTANT, NOT A SEARCH. The owner's instruction: "go run
// some seeds (in C not the simulator) and try to find a replay code that ends up
// with a bunch of cards on the table. Then simply feed that replay code to the
// seeded fixed state, seat yourself as defender, and hit pickup. THAT SIMPLE."
//
// That is exactly what this does, and it is better than the alternative in every
// way that matters. `c/tests/msg_wire_test --fatboard 10 2` searches deals in
// microseconds and prints ONE FMSG envelope as hex; this file just reads that
// hex and opens it. There is no search logic on the device, no dependence on
// what the deal happens to allow, and the same board comes up every single run -
// which is what makes a filmed before/after comparable at all.
//
// (An earlier version played the game forward through the kernel ON DEVICE at
// launch. It worked, and it was the wrong shape: slower, non-deterministic
// across deals, and it put a legality search in the extension to produce
// something a build-time constant could state outright.)
//
// IT ALSO SKIPS THE WHOLE FLOW. With the flag set, the extension does not show
// setup, does not create a lobby, does not join or start: `load()` opens this
// chain directly, seated as the DEFENDER, so the entire verification run is
// "open the extension, hit Pickup, record" (owner: "use build flags to skip the
// create game / join game / start game stuff and jump straight to the game
// state").
//
// HOW TO USE IT (simulator or device):
//   c/build/msg_wire_test --fatboard 10 2 > /tmp/fat.hex
//   cp /tmp/fat.hex <AppGroup>/dev.fatboard
//   …then just open the extension.
// `ios/Tools/msgrig.sh fatboard` does all of it, including the search.
//
// A FILE, not a UserDefaults key, for the same reason `dev.seed` is one:
// `defaults write` from outside the sandbox lands in the wrong domain, and
// cfprefsd caches App Group preferences until a reboot.

#if DEBUG || SOLO_TESTING
import Foundation

public enum MessageDevBoard {
    private static let appGroup = "group.cards.foolish.msg"
    private static let flagFile = "dev.fatboard"
    private static let seatFile = "dev.seat"

    /// The seeded chain, or nil when the flag file is absent - which is the
    /// normal case, including every ordinary DEBUG run.
    ///
    /// Hex, because it survives a shell pipeline, a `cp`, and an editor without
    /// anyone having to agree on a base32 alphabet or a padding rule; the C side
    /// prints it and this reads it, and there is no third opinion.
    public static var seededPayload: Data? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(flagFile),
                                    encoding: .utf8)
        else { return nil }
        return hex(raw.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Which seat to sit at, or nil to sit at the defender's.
    ///
    /// The default suits the pickup case (only the defender may pick up), but
    /// the DEAL case needs the opposite chair: it is an ATTACKER saying good
    /// that closes the bout and triggers the round transition, and the deal is
    /// what the animation under test belongs to.
    public static var seededSeat: Int? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(seatFile),
                                    encoding: .utf8),
              let n = Int(raw.trimmingCharacters(in: .whitespacesAndNewlines))
        else { return nil }
        return n
    }

    /// Even-length hex to bytes; nil on anything malformed, so a truncated or
    /// half-written file reads as "no seed" rather than as a damaged game.
    private static func hex(_ s: String) -> Data? {
        let chars = Array(s.utf8)
        guard chars.count >= 2, chars.count % 2 == 0 else { return nil }
        var out = Data(capacity: chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = nibble(chars[i]), let lo = nibble(chars[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
            i += 2
        }
        return out
    }

    private static func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30              // 0-9
        case 0x61...0x66: return c - 0x61 + 10         // a-f
        case 0x41...0x46: return c - 0x41 + 10         // A-F
        default: return nil
        }
    }
}
#endif
