#if UTTT_BIG_BOARD
import CUttt
import CoreGraphics
import Foundation
import Security

/// The big game's kernel, and nothing else (docs/BIG_BOARD.md).
///
/// A 243 x 243 recursive board whose bubble carries a format-3 link and the
/// board as its picture. Every rule, seat and word comes from C
/// (uttt/c/src/uttt_big_msg.h, the "THE 243 BOARD" section of uttt_api.h);
/// like `Uttt`, this file forwards and answers no question of its own.
///
/// THE WHOLE FILE IS UNDER `UTTT_BIG_BOARD`: a build without the condition
/// compiles none of it, names none of the kernel's `uti_big_*` entries, and so
/// links none of them (they are hidden in the kernel and dead-stripped).
public enum UtttBig {

    /// THE TESTFLIGHT-ONLY RULE, its runtime half (docs/BIG_BOARD.md). The
    /// compile-time half is the condition this file sits under, which an
    /// App Store archive leaves out; this half keeps a build that has it
    /// from offering the big game anywhere but a debug build or a TestFlight
    /// install (whose receipt is the sandbox one).
    public static var available: Bool {
#if DEBUG
        true
#else
        Bundle.main.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt"
#endif
    }

    /// Cells a side, and on the board.
    public static let side = Int(UTI_BIG_SIDE)
    public static let cellCount = Int(UTI_BIG_CELLS)

    /// Is `text` a big-game link: the router's question, by its format byte.
    public static func isBig(_ text: String) -> Bool { uti_big_is(text) != 0 }

    /// A new big invitation from me, composed now, on a napkin from the
    /// system's secure random - as `Uttt.openInvitation`.
    public static func openInvitation(at date: Date = Date()) {
        uti_big_open(Int64(date.timeIntervalSince1970), Int32(randomLook()))
    }

    /// AGAIN after the resident finished big game, on its napkin. False, and
    /// nothing changed, while it runs.
    @discardableResult
    public static func openRematch(at date: Date = Date()) -> Bool {
        uti_big_open_again(Int64(date.timeIntervalSince1970)) != 0
    }

    /// One byte of secure random: SecRandomCopyBytes, and the system
    /// generator should it ever refuse. (Uttt's own, copied: neither file
    /// reaches into the other.)
    private static func randomLook() -> UInt8 {
        var b: UInt8 = 0
        let ok = withUnsafeMutableBytes(of: &b) { SecRandomCopyBytes(kSecRandomDefault, 1, $0.baseAddress!) }
        return ok == errSecSuccess ? b : UInt8.random(in: .min ... .max)
    }

    /// Adopt a big message: its link and the cells its picture read back to.
    /// False, and nothing changed, if the kernel refuses them - or if `cells`
    /// is not a whole board, which the kernel would read past.
    @discardableResult
    public static func read(_ text: String, cells: [UInt8]) -> Bool {
        guard cells.count == cellCount else { return false }
        return uti_big_read(text, cells) == 0
    }

    /// The resident big message, as the link a bubble carries.
    public static var messageText: String? {
        var buf = [CChar](repeating: 0, count: Int(UTI_BIG_TEXT_MAX))
        let n = buf.withUnsafeMutableBufferPointer {
            uti_big_text($0.baseAddress, Int32(UTI_BIG_TEXT_MAX))
        }
        return n > 0 ? String(cString: buf) : nil
    }

    /// The resident board, a copy: `cellCount` symbols (0 empty, 1 X, 2 O)
    /// in leaf order - the picture's payload.
    public static var cells: [UInt8] {
        guard let p = uti_big_cells() else { return [] }
        return Array(UnsafeBufferPointer(start: p, count: cellCount))
    }

    public static var seat: Uttt.Seat { Uttt.Seat(uti_big_seat()) }
    /// The mark this device plays, or `.none`.
    public static var myMark: Uttt.Mark { Uttt.Mark(rawValue: UInt8(uti_big_mark())) ?? .none }
    public static var seed: Int32 { uti_big_seed() }
    public static var look: UInt8 { UInt8(truncatingIfNeeded: uti_big_look()) }
    public static var canMove: Bool { uti_big_can_move() != 0 }
    public static var sealed: Bool { uti_big_sealed() != 0 }

    public static var over: Uttt.Mark { Uttt.Mark(rawValue: UInt8(uti_big_over())) ?? .none }
    public static var turn: Uttt.Mark { Uttt.Mark(rawValue: UInt8(uti_big_turn())) ?? .none }
    public static var plyCount: Int { Int(uti_big_n_plies()) }
    /// The last move, -1 for none.
    public static var last: Int { Int(uti_big_last()) }
    /// Where the next mark must go: a node id (0 anywhere), -1 when over.
    public static var region: Int { Int(uti_big_region()) }

    /// Node `id`'s status, its level (0 the root), and its rectangle in the
    /// board's 0..1 square.
    public static func node(_ id: Int) -> Uttt.Mark {
        Uttt.Mark(rawValue: UInt8(max(0, uti_big_node(Int32(id))))) ?? .none
    }
    public static func nodeLevel(_ id: Int) -> Int { Int(uti_big_node_level(Int32(id))) }
    public static func nodeRect(_ id: Int) -> CGRect {
        var r: [Float] = [0, 0, 0, 0]
        guard uti_big_node_rect(Int32(id), &r) != 0 else { return .zero }
        return CGRect(x: CGFloat(r[0]), y: CGFloat(r[1]), width: CGFloat(r[2]), height: CGFloat(r[3]))
    }

    /// The square leaf `mv` covers in the board's 0..1 square - the
    /// rectangle `hit` maps back to it.
    public static func cellRect(_ mv: Int) -> CGRect {
        var r: [Float] = [0, 0, 0, 0]
        guard uti_big_cell_rect(Int32(mv), &r) != 0 else { return .zero }
        return CGRect(x: CGFloat(r[0]), y: CGFloat(r[1]), width: CGFloat(r[2]), height: CGFloat(r[3]))
    }

    /// The leaf under a point in the board's 0..1 square, or nil off it.
    public static func hit(_ p: CGPoint) -> Int? {
        let mv = uti_big_hit(Float(p.x), Float(p.y))
        return mv >= 0 ? Int(mv) : nil
    }

    /// Play as me. On an open invitation this TAKES THE SEAT with the move.
    @discardableResult
    public static func playAsMe(_ move: Int) -> Bool { uti_big_play(Int32(move)) != 0 }
    /// A change of mind: may I replace my staged move with `move`. Pure.
    public static func canReplace(_ move: Int) -> Bool { uti_big_can_replace(Int32(move)) != 0 }
    /// Take back my own last move; the joining move gives the seat back.
    @discardableResult
    public static func undoMine() -> Bool { uti_big_undo() != 0 }

    /// The one door a screen may offer: Again, at the end.
    public static var door: Uttt.Door { uti_big_door() == UTI_DOOR_AGAIN ? .again : .none }

    /// Which to show: true for `mine` (the staged draft), false for `tapped`.
    public static func prefersMine(_ mine: String, over tapped: String) -> Bool {
        uti_big_prefer(mine, tapped) <= 0
    }

    public static func sameGame(_ a: String, _ b: String) -> Bool {
        uti_big_same_game(a, b) != 0
    }

    /// THE SENDER FACT about the big message in `text`, as `Uttt.sender`.
    /// Nil clears it.
    public static func sender(of text: String?, isDM: Bool = false, iSent: Bool = false) {
        guard let text else { uti_big_sender(nil, 0, -1); return }
        uti_big_sender(text, isDM ? 1 : 0, iSent ? 1 : 0)
    }

    /// This device's record for the resident big game: .x, .o, or nil.
    public static var record: Uttt.Seat? {
        let r = uti_big_record()
        return r == 0 ? nil : Uttt.Seat(r)
    }

    /// Which witness seated me on the resident big game.
    public static var seatBy: Uttt.Witness { Uttt.Witness(rawValue: uti_big_seat_by()) ?? .none }

    /// The bubble's caption: the 9 x 9's words exactly.
    public static var caption: String {
        var buf = [CChar](repeating: 0, count: 96)
        let n = buf.withUnsafeMutableBufferPointer { uti_big_caption($0.baseAddress, 96) }
        return n > 0 ? String(cString: buf) : ""
    }

    /// The screen's words for the big resident (the keys the kernel answers
    /// for it are listed at uti_big_say; any other is "").
    public static func say(_ s: Uttt.Say) -> String { String(cString: uti_big_say(s.key)) }

    /// The mark drawn inside the play-surface headline, or `.none`.
    public static var sayMark: Uttt.Mark { Uttt.Mark(rawValue: UInt8(uti_big_say_mark())) ?? .none }
    /// The mark the bubble's headline draws (the winner's), or `.none`.
    public static var bubbleMark: Uttt.Mark { Uttt.Mark(rawValue: UInt8(uti_big_say_bubble_mark())) ?? .none }
}
#endif
