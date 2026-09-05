// BoardWire.swift - the board's own sets and small decisions, asked of the
// kernel.
//
// The veil (which cards the board must render as not-yet-there), the window it
// is up for, what a finishing sequence and a new play owe each other, what a tap
// resolves to, who may say what the badges are showing, and the order the end
// screen ranks people in. These were the last pure statics on
// MessageTableView.swift; they give the same answers on any screen, so they are
// c/src/anim_plan.c's now and this is the crossing.
//
// WHAT DID NOT COME WITH THEM: rects, angles, springs, and anything measured
// against the host - the drag hint's position, the collapse fraction, the
// landing rects, the flight builders. That is rendering, and it stays on the
// board where it belongs.
//
// CARD SETS CROSS AS A u64 BITSET over dense ids, the representation the
// conflict model already uses: a deck fits in 64 bits, so a union is an or and a
// membership test is a shift. That is why none of these needs a packed record -
// a versioned envelope around two u64s would be ceremony, and the brief says so.

import Foundation
import CFoolish

/// The crossing for a set of card identities.
///
/// The board thinks in `Card.identity` strings because that is what
/// `matchedGeometryEffect` is keyed on; the kernel thinks in dense ids 0..51.
/// This is the one place the two meet, so "which card is bit 17" has a single
/// answer and a second client cannot drift from it.
///
/// A CARD OUTSIDE THE DECK HAS NO BIT, and that is not a loss. The only such
/// identity is a viewer-masked back, which the board never veils in the first
/// place - a back has no identity to hide, it renders as a back regardless (see
/// `MessageTurnController.openReplayTouchedCardIds`, which drops them at the
/// source). It is the same thing the conflict model says about a masked card.
public enum CardSet {

    /// Dense id -> the identity string the board keys its views on.
    static let identities: [String] = (0..<52).map { Card(s: $0 / 13, v: $0 % 13 + 1).identity }

    /// ...and back. Built off `identities` rather than by parsing, so the two
    /// directions cannot disagree.
    static let byIdentity: [String: UInt8] = {
        var m: [String: UInt8] = [:]
        for (i, id) in identities.enumerated() { m[id] = UInt8(i) }
        return m
    }()

    /// A card's dense id, or nil for a masked back or anything off the deck.
    public static func id(of card: Card) -> UInt8? {
        guard !card.isHidden, (0...3).contains(card.s), (1...13).contains(card.v) else { return nil }
        return UInt8(card.s * 13 + (card.v - 1))
    }

    /// The card a dense id names.
    public static func card(_ id: UInt8) -> Card { Card(s: Int(id) / 13, v: Int(id) % 13 + 1) }

    static func bits(_ ids: Set<String>) -> UInt64 {
        var bits: UInt64 = 0
        for id in ids { if let n = byIdentity[id] { bits |= 1 << UInt64(n) } }
        return bits
    }

    static func bits(_ cards: [Card]) -> UInt64 {
        var bits: UInt64 = 0
        for c in cards { if let n = id(of: c) { bits |= 1 << UInt64(n) } }
        return bits
    }

    static func identities(_ bits: UInt64) -> Set<String> {
        var out = Set<String>()
        var rest = bits
        while rest != 0 {
            let i = rest.trailingZeroBitCount
            rest &= rest - 1
            if i < 52 { out.insert(identities[i]) }
        }
        return out
    }

    /// The dense ids of an ordered hand, dropping anything that is not a card.
    /// Order is the point wherever this is used - the fan places by index.
    static func ids(_ cards: [Card]) -> [UInt8] { cards.compactMap(id(of:)) }
}

/// THE VEIL: every card the board must render as not-yet-there, and the three
/// sets derived from it. See anim_plan.h for each rule; the short of it is that
/// the veil is a union of three sources and everything else is a subtraction.
public enum Veil {

    /// Every card the board renders as not-yet-there - the set the battle grid
    /// and the hand fan are both built from, so "is this card on the table" has
    /// one answer. `myHand` nil is a spectator or a board with no view yet, and
    /// takes the live-play source out entirely.
    public static func veiled(hidden: Set<String>, pendingOpen: Set<String>?,
                              handBeforeMyMove: Set<String>?, myHand: [Card]?) -> Set<String> {
        CardSet.identities(fio_veil_veiled(CardSet.bits(hidden),
                                           CardSet.bits(pendingOpen ?? []),
                                           handBeforeMyMove == nil ? 0 : 1,
                                           CardSet.bits(handBeforeMyMove ?? []),
                                           myHand == nil ? 0 : 1,
                                           CardSet.bits(myHand ?? [])))
    }

    /// "In the air right now": the animator's `hidden \ preHidden`. Four things
    /// rest on it - the cards that keep their fan slot, the covers whose attack
    /// tilts with them, and the orphan test at a teardown.
    public static func flying(hidden: Set<String>, preHidden: Set<String>) -> Set<String> {
        CardSet.identities(fio_veil_flying(CardSet.bits(hidden), CardSet.bits(preHidden)))
    }

    /// Which veiled hand cards reserve NO fan width yet. Without this the fan
    /// would slide left in anticipation of a deal while unrelated earlier steps
    /// are still playing.
    public static func handSlotDeferred(veiled: Set<String>, flying: Set<String>,
                                        holdback: [Card]) -> Set<String> {
        CardSet.identities(fio_veil_hand_slot_deferred(CardSet.bits(veiled),
                                                       CardSet.bits(flying),
                                                       CardSet.bits(holdback)))
    }

    /// Which veiled cards the fan draws nothing for - `hand(_:)`'s `hidden:`
    /// argument, and the twin of `handSlotDeferred` one step out.
    public static func fan(veiled: Set<String>, holdback: [Card]) -> Set<String> {
        CardSet.identities(fio_veil_fan(CardSet.bits(veiled), CardSet.bits(holdback)))
    }

    /// What the battle grid is told, once the caller knows which table it is
    /// drawing. The two branches answer with different state rather than a
    /// different filter over one - see anim_plan.h for the pickup that makes
    /// that necessary.
    public static func grid(sweeping: Bool, veiled: Set<String>,
                            sweptFlown: Set<String>, sweepUnplaced: Set<String>,
                            sweepArriving: Set<String>, flying: Set<String>)
        -> (hidden: Set<String>, flyingNow: Set<String>) {
        var hidden: UInt64 = 0, air: UInt64 = 0
        fio_veil_grid(sweeping ? 1 : 0, CardSet.bits(veiled), CardSet.bits(sweptFlown),
                      CardSet.bits(sweepUnplaced), CardSet.bits(sweepArriving),
                      CardSet.bits(flying), &hidden, &air)
        return (hidden: CardSet.identities(hidden), flyingNow: CardSet.identities(air))
    }

    /// What a finishing sequence owes the board: which opened-but-unflown cards
    /// to reveal now, and which to leave for whoever is still running.
    public static func teardown(opened: Set<String>, orphaned: Set<String>,
                                isNewest: Bool) -> (reveal: Set<String>, carry: Set<String>) {
        var reveal: UInt64 = 0, carry: UInt64 = 0
        fio_veil_teardown(CardSet.bits(opened), CardSet.bits(orphaned),
                          isNewest ? 1 : 0, &reveal, &carry)
        return (reveal: CardSet.identities(reveal), carry: CardSet.identities(carry))
    }

    /// What a new play owes the one it replaces: the pending-placement ledger
    /// holds one slot, so a play that finds an earlier one standing has to take
    /// that veil down before raising its own.
    public static func handover(standing: Set<String>, placing: Set<String>)
        -> (reveal: Set<String>, veil: Set<String>) {
        var reveal: UInt64 = 0, veil: UInt64 = 0
        fio_veil_handover(CardSet.bits(standing), CardSet.bits(placing), &reveal, &veil)
        return (reveal: CardSet.identities(reveal), veil: CardSet.identities(veil))
    }

    /// The window the veil is up for: a replay the board has been handed and has
    /// not begun to animate. Both halves matter - ignoring the pending flag
    /// makes the veil never lift, which no test used to notice.
    public static func unstartedReplay(replayPending: Bool, events: [GameEvent]) -> [GameEvent]? {
        fio_veil_unstarted_replay(replayPending ? 1 : 0, Int32(events.count)) != 0 ? events : nil
    }

    /// Does this teardown own the holdback? Only if the holdback was armed no
    /// later than the veil the teardown raised.
    public static func holdbackIsMine(armedAt: Int, teardownAt: Int) -> Bool {
        fio_holdback_is_mine(Int32(armedAt), Int32(teardownAt)) != 0
    }

    /// The selection after a tap, which may only ever name cards in my hand. A
    /// card drawn in the fan but no longer in the kernel hand (the holdback) can
    /// neither enter the selection nor survive in it.
    public static func selectionAfterTap(_ selection: Set<String>, card: Card,
                                         hand: [Card]) -> Set<String> {
        CardSet.identities(fio_selection_after_tap(CardSet.bits(selection),
                                                   Int32(CardSet.id(of: card).map(Int.init) ?? -1),
                                                   CardSet.bits(hand)))
    }
}

/// WHO MAY SAY WHAT THE BADGES ARE SHOWING.
///
/// The shown counts, the out badges and the role marks belong to whoever is
/// ANIMATING: a running sequence freezes them to the board before its move and
/// walks them forward one step per landing flight. A caller that is not that
/// sequence must not write them, or every badge snaps to a value the cards on
/// screen have not earned and the stream's next step puts it back.
///
/// The CLAIM is the caller's own name for itself and stays where the callers
/// are (`ShownClaim` in ShownLedger.swift, which documents which caller is
/// which); the RULE over it is the kernel's, and these are the codes the two
/// agree on.
public enum ShownWrite {
    public static let sequence  = Int(FIO_CLAIM_SEQUENCE)
    public static let arming    = Int(FIO_CLAIM_ARMING)
    public static let handOff   = Int(FIO_CLAIM_HAND_OFF)
    public static let bystander = Int(FIO_CLAIM_BYSTANDER)

    /// Only a bystander ever stands down, and only while a sequence is running.
    /// Guarding the owner's own advance would freeze every badge for the life of
    /// the board, which is a far worse defect than the twitch.
    public static func allows(claim: Int, sequencing: Bool) -> Bool {
        fio_shown_ledger_allows(Int32(claim), sequencing ? 1 : 0) != 0
    }
}

/// One line of the end screen, before a name is attached to it. The NAME is the
/// client's: identity lives in the roster and is localized, which is the same
/// reason no other wire in the SDK carries one.
public struct FinishPlace: Equatable, Sendable {
    public let place: Int
    public let total: Int
    public let seat: Int
    public let isYou: Bool
    /// The fool is the one seat still holding cards, and takes the last place.
    public var isFool: Bool { place == total }

    public init(place: Int, total: Int, seat: Int, isYou: Bool) {
        self.place = place
        self.total = total
        self.seat = seat
        self.isYou = isYou
    }
}

public enum FinishOrder {

    /// The finish order: rank 1 is the first player out, counting up to the fool
    /// last. A spectator (`mySeat` -1) owns no row.
    public static func places(_ view: GameView, mySeat: Int) -> [FinishPlace] {
        // A seat off the roster is refused outright rather than dropped: a
        // dropped entry would shift every rank below it by one, which is a
        // wrong answer where an empty screen is an obvious one.
        guard view.eliminationOrder.allSatisfy({ (0..<8).contains($0) }) else { return [] }
        let elimination = view.eliminationOrder.map { UInt8($0) }
        var out = [CChar](repeating: 0, count: Int(FIO_FINISH_HEAD) + 3 * 8)
        let n: Int32 = elimination.withUnsafeBufferPointer { p in
            fio_finish_rows(p.baseAddress, Int32(elimination.count), Int32(view.gameOver),
                            Int32(view.players.count), Int32(mySeat), &out, Int32(out.count))
        }
        guard n >= Int32(FIO_FINISH_HEAD) else { return [] }
        let b = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        let rows = Int(b[1]), total = Int(b[2])
        guard b.count >= Int(FIO_FINISH_HEAD) + 3 * rows else { return [] }
        return (0..<rows).map { i in
            let at = Int(FIO_FINISH_HEAD) + 3 * i
            return FinishPlace(place: Int(b[at]), total: total,
                               seat: Int(b[at + 1]), isYou: b[at + 2] != 0)
        }
    }
}
