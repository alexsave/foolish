// The Swift face of the C kernel. Thin on purpose.
//
// Every rule in this product is in C (c/src/ww_*.c) and every one of these
// methods is a forward. What Swift is allowed to do is lay out pixels and talk
// to Messages; it is not allowed to decide who is a wolf, whose view carries
// what, which chain wins a race, or when Send is legal - all of which have
// exactly one implementation, over there, with the tests.
//
// Nothing here parses a layout. The bridge exposes flat accessors rather than a
// blob (c/ios/include/ww_api.h says why), so there is no byte offset in this file
// to get wrong against a kernel that moved.
import CWerewolf
import Foundation

public enum Role: Int32, Sendable {
    case villager = 0
    case wolf = 1
    case seer = 2
    case unknown = 255
}

public enum Team: Int32, Sendable {
    case village = 0
    case wolves = 1
    case none = 255
}

public enum Phase: Int32, Sendable {
    case lobby = 0
    case night = 1
    case day = 2
    case over = 3
}

/// What a third seat learns about one seat's night record. Three values, and none
/// of them is the target or the role - see `ww_view.h` contract 1.
public enum Sent: Int32, Sendable {
    case no = 0
    case yes = 1
    /// A later player carried this seat's pass forward. Public on purpose: the
    /// carrier's own bubble already says whose passes it brought.
    case carried = 2
}

public struct KernelError: Error, CustomStringConvertible {
    public let code: Int32
    public var description: String { "werewolf kernel refused: \(code)" }
}

/// No seat. 255 on the wire, and the same 255 here rather than an Optional,
/// because it crosses the boundary as a byte and one representation is fewer
/// places to convert it wrong.
public let noSeat: Int = 255

/// THE resident session. One game, held by the C side (`ww_api.h`), so this is a
/// facade over a singleton rather than a value.
///
/// NEVER SEAL OR READ ACROSS AN `await`. The fork shipped that bug and paid for
/// it: decoding IS adopting, so an await in the middle of one leaves the device
/// half on a game it may then send.
public final class Kernel {
    public static let shared = Kernel()
    private init() {}

    public let sendFloorSeconds = 10
    public let minPlayers = 5
    public let maxPlayers = 10
    public let chatMax = 40

    // ------------------------------------------------------------ session ---

    /// Deal a fresh game. `seed` must be 32 bytes.
    public func newGame(seed: Data, players: Int) throws {
        precondition(seed.count == 32, "the deal seed is 32 bytes")
        let rc = seed.withUnsafeBytes { wwi_new_game($0.bindMemory(to: UInt8.self).baseAddress, Int32(players)) }
        if rc != 0 { throw KernelError(code: rc) }
    }

    /// A fresh 32-byte seed from the system CSPRNG. The whole hidden deal is this
    /// value, so it comes from the OS and nowhere else.
    public static func freshSeed() -> Data {
        var s = Data(count: 32)
        _ = s.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, 32, $0.baseAddress!) }
        return s
    }

    /// Decode AND replay a bubble. Throws on a damaged payload, and the device is
    /// left on the game it was already showing.
    public func adopt(_ payload: Data) throws {
        let rc = payload.withUnsafeBytes {
            wwi_adopt($0.bindMemory(to: UInt8.self).baseAddress, Int32(payload.count))
        }
        if rc != 0 { throw KernelError(code: rc) }
    }

    /// Rule P. `true` when `incoming` should replace `showing`.
    public func prefers(incoming: Data, over showing: Data) -> Bool {
        incoming.withUnsafeBytes { a in
            showing.withUnsafeBytes { b in
                wwi_prefer(a.bindMemory(to: UInt8.self).baseAddress, Int32(incoming.count),
                           b.bindMemory(to: UInt8.self).baseAddress, Int32(showing.count)) < 0
            }
        }
    }

    /// Seal the resident game into a bubble. `parent` is the previous bubble's
    /// exact bytes - the bridge hashes them, because a caller that hashes a
    /// re-serialized copy of "the same" envelope commits to a different chain.
    public func seal(gameId: UInt64, sentAt: UInt16, parent: Data?) throws -> Data {
        var out = Data(count: 2048)
        let n: Int32 = out.withUnsafeMutableBytes { o in
            let dst = o.bindMemory(to: UInt8.self).baseAddress
            guard let parent, !parent.isEmpty else {
                return wwi_seal(dst, 2048, gameId, sentAt, nil, 0)
            }
            return parent.withUnsafeBytes { p in
                wwi_seal(dst, 2048, gameId, sentAt,
                         p.bindMemory(to: UInt8.self).baseAddress, Int32(parent.count))
            }
        }
        if n <= 0 { throw KernelError(code: n) }
        out.removeSubrange(Int(n)..<out.count)
        return out
    }

    // ------------------------------------------------------------- roster ---

    public func setRoster(seat: Int, name: String) throws {
        var bytes = Array(name.utf8)
        if bytes.count > 16 { bytes = Array(bytes.prefix(16)) }
        let rc = bytes.withUnsafeBufferPointer { wwi_roster_set(Int32(seat), $0.baseAddress, Int32(bytes.count)) }
        if rc != 0 { throw KernelError(code: rc) }
    }

    public var rosterCount: Int { Int(wwi_roster_count()) }

    public func rosterName(_ i: Int) -> String? {
        var buf = [UInt8](repeating: 0, count: 16)
        let n = buf.withUnsafeMutableBufferPointer { wwi_roster_name(Int32(i), $0.baseAddress, 16) }
        guard n > 0 else { return nil }
        return String(decoding: buf.prefix(Int(n)))
    }

    public func rosterSeat(_ i: Int) -> Int { Int(wwi_roster_seat(Int32(i))) }

    /// Which seat is this device? The board's answer and the lobby's differ, and
    /// the difference is a rule (`ww_seat.h`), so there are two calls and not a
    /// flag a caller can get backwards.
    public func seatOnBoard(cached: Int, senderIsLocal: Bool, lastActor: Int,
                            chatIsDM: Bool, myName: String) -> Int {
        let bytes = Array(myName.utf8)
        return Int(bytes.withUnsafeBufferPointer {
            wwi_seat_on_board(Int32(cached), senderIsLocal ? 1 : 0, Int32(lastActor),
                              chatIsDM ? 1 : 0, $0.baseAddress, Int32(bytes.count))
        })
    }

    public func seatInLobby(cached: Int, senderIsLocal: Bool, lastActor: Int,
                            chatIsDM: Bool, myName: String) -> Int {
        let bytes = Array(myName.utf8)
        return Int(bytes.withUnsafeBufferPointer {
            wwi_seat_in_lobby(Int32(cached), senderIsLocal ? 1 : 0, Int32(lastActor),
                              chatIsDM ? 1 : 0, $0.baseAddress, Int32(bytes.count))
        })
    }

    // -------------------------------------------------------------- night ---

    /// One night record. `line` is the wolves' channel and is REFUSED from a
    /// non-wolf rather than dropped - a client that believes it sent a line and
    /// did not is a client that will send a second bubble.
    public func nightAct(seat: Int, target: Int, line: String? = nil, carry: Bool = false) throws {
        var bytes = Array((line ?? "").utf8)
        if bytes.count > chatMax { bytes = Array(bytes.prefix(chatMax)) }
        let rc = bytes.withUnsafeBufferPointer {
            wwi_night_act(Int32(seat), Int32(target),
                          bytes.isEmpty ? nil : $0.baseAddress, Int32(bytes.count),
                          carry ? 1 : 0)
        }
        if rc != 0 { throw KernelError(code: rc) }
    }

    public func dayLynch(target: Int) throws {
        let rc = wwi_day_lynch(Int32(target))
        if rc != 0 { throw KernelError(code: rc) }
    }

    /// Seconds before Send may be tapped. A kernel rule, not a view's opinion:
    /// the floor is what removes the instant-answer tell, and a client that set
    /// its own number would be a client where villagers answer faster.
    public func sendFloorRemaining(openedAt: UInt16, now: UInt16) -> Int {
        Int(wwi_send_floor_remaining(openedAt, now))
    }

    public func mayCarry(lastSealAt: UInt16, now: UInt16) -> Bool {
        wwi_may_carry(lastSealAt, now) != 0
    }

    // --------------------------------------------------------------- view ---
    //
    // `viewer` is a seat, or -1 for a spectator. Every one of these reads the
    // masked blob for that viewer on the C side.

    public func phase(_ viewer: Int) -> Phase { Phase(rawValue: wwi_view_phase(Int32(viewer))) ?? .lobby }
    public func night(_ viewer: Int) -> Int { Int(wwi_view_night(Int32(viewer))) }
    public func playerCount(_ viewer: Int) -> Int { Int(wwi_view_n_players(Int32(viewer))) }
    public func turn(_ viewer: Int) -> Int { Int(wwi_view_turn(Int32(viewer))) }
    public func winner(_ viewer: Int) -> Team { Team(rawValue: wwi_view_winner(Int32(viewer))) ?? .none }
    public func isAlive(_ viewer: Int, _ seat: Int) -> Bool { wwi_view_alive(Int32(viewer), Int32(seat)) != 0 }
    public func myRole(_ viewer: Int) -> Role { Role(rawValue: wwi_view_my_role(Int32(viewer))) ?? .unknown }
    public func role(_ viewer: Int, of seat: Int) -> Role {
        Role(rawValue: wwi_view_role_of(Int32(viewer), Int32(seat))) ?? .unknown
    }
    public func sent(_ viewer: Int, _ seat: Int) -> Sent {
        Sent(rawValue: wwi_view_sent(Int32(viewer), Int32(seat))) ?? .no
    }
    public func ownTarget(_ viewer: Int) -> Int { Int(wwi_view_own_target(Int32(viewer))) }
    public func decider(_ viewer: Int) -> Int { Int(wwi_view_decider(Int32(viewer))) }
    public func chatCount(_ viewer: Int) -> Int { Int(wwi_view_chat_count(Int32(viewer))) }
    public func chatSeat(_ viewer: Int, _ i: Int) -> Int { Int(wwi_view_chat_seat(Int32(viewer), Int32(i))) }
    public func chatLine(_ viewer: Int, _ i: Int) -> String? {
        var buf = [UInt8](repeating: 0, count: 64)
        let n = buf.withUnsafeMutableBufferPointer {
            wwi_view_chat_line(Int32(viewer), Int32(i), $0.baseAddress, 64)
        }
        guard n > 0 else { return nil }
        return String(decoding: buf.prefix(Int(n)))
    }
    public func reading(_ viewer: Int, of seat: Int) -> Team {
        Team(rawValue: wwi_view_reading(Int32(viewer), Int32(seat))) ?? .none
    }
    public func victim(_ viewer: Int, night: Int) -> Int { Int(wwi_view_victim(Int32(viewer), Int32(night))) }
    public func lynched(_ viewer: Int, night: Int) -> Int { Int(wwi_view_lynched(Int32(viewer), Int32(night))) }
}

private extension String {
    /// UTF-8 in, and a replacement-character fallback rather than a crash: the
    /// bytes come off a wire an attacker can write.
    init<S: Sequence>(decoding bytes: S) where S.Element == UInt8 {
        self = String(decoding: Array(bytes), as: UTF8.self)
    }
}
