// TurnWire.swift - the chain layer's turn, asked of the kernel.
//
// A turn on a chain is staged, not played: a device establishes a base, applies
// its own actions locally, and seals the result into a bubble the human presses
// Send on. The controller that drives that
// (FoolishKit/Messages/MessageTurnController) is a state machine over time, and
// its suspension points are Swift's - an actor hop, a decode, a paint. Those
// stay there.
//
// The DECISIONS ACROSS them are the kernel's (c/src/msg_wire.c, msg_turn_*) and
// this file is the crossing. Facts in, an answer out; the caller performs the
// effect. It is the same division BotDriveWire already crosses: the kernel says
// which action and how long to wait, and the host decides only how to wait.
//
// It is a file in sdk/swift rather than an import up in Messages because the C
// bridge stays here (§7.1, ios/scripts/lint_architecture.sh): the app layer
// meets typed Swift instead of Int32 sentinels. Everything crosses as ints -
// the state is eight booleans and every answer is a small enum, so a packed
// record around either would be ceremony.

import Foundation
import CFoolish

public enum TurnWire {

    /// THE CHAIN STATE. Every bit is a fact about this device's turn, never
    /// about the game: the game is the state blob and no rule here reads it.
    public struct State: OptionSet, Sendable, Equatable {
        public let rawValue: Int32
        public init(rawValue: Int32) { self.rawValue = rawValue }

        /// Actions applied locally that are not yet in the thread.
        public static let staged        = State(rawValue: Int32(FIO_TURN_STAGED))
        /// Send was pressed and the rebase has not resolved. The bytes are
        /// already on their way, so nothing may be undone or re-sent in it.
        public static let sending       = State(rawValue: Int32(FIO_TURN_SENDING))
        /// `begin()` has established a base chain at least once.
        public static let ready         = State(rawValue: Int32(FIO_TURN_READY))
        /// This board branches off a chain the table has moved past - read-only.
        public static let superseded    = State(rawValue: Int32(FIO_TURN_SUPERSEDED))
        /// A conflict retraction is in flight.
        public static let retracting    = State(rawValue: Int32(FIO_TURN_RETRACTING))
        /// A live board is mounted, so there is somebody to fly a retraction.
        public static let boardWatching = State(rawValue: Int32(FIO_TURN_BOARD_WATCHING))
        /// A bout settlement is withheld until Send.
        public static let held          = State(rawValue: Int32(FIO_TURN_HELD))
        /// A dealt game with no parent chain.
        public static let genesis       = State(rawValue: Int32(FIO_TURN_GENESIS))
    }

    // MARK: - what may be staged

    /// Is there a staged bubble to send? False inside the send window, which
    /// has already claimed those bytes.
    public static func canSend(_ s: State) -> Bool {
        fio_msg_turn_can_send(s.rawValue) != 0
    }

    /// May this seat act at all? `humanMoves` is the count from the HUMAN menu
    /// (`PlayWire.humanMoves`), never the raw one: the raw menu always offers
    /// `good`, a human may not say good over an uncovered attack, and a handoff
    /// counting the raw menu calls it this seat's move with no button on screen.
    public static func canAct(_ s: State, humanMoves: Int) -> Bool {
        fio_msg_turn_can_act(s.rawValue, Int32(humanMoves)) != 0
    }

    /// Is there a bubble to put in the input field? Either I staged one, or this
    /// is a genesis on which I have no move at all and the deal itself is what
    /// has to travel.
    public static func canStage(_ s: State, humanMoves: Int) -> Bool {
        fio_msg_turn_can_stage(s.rawValue, Int32(humanMoves)) != 0
    }

    // MARK: - the door every gesture comes through

    public enum Admission: Int32, Sendable {
        case ok = 0
        /// SILENT. The window is one red flight long and the tap simply does
        /// nothing, exactly as it would a frame later with the arrival up.
        case retracting = 1
        case superseded = 2
        case heldPickup = 3
    }

    /// May this move be applied? Enforced as well as displayed, because this is
    /// the one door every gesture, dev path and future shortcut comes through.
    public static func admit(_ s: State, move: Move, pickupHold: Int) -> Admission {
        Admission(rawValue: fio_msg_turn_admit(s.rawValue,
                                               Int32(MoveWire.wireIndex(move.type)),
                                               Int32(pickupHold))) ?? .ok
    }

    // MARK: - a chain that arrived

    public enum Arrival: Int32, Sendable {
        /// The chain I am already on. Staged moves STAND - they were composed
        /// against exactly these bytes, and a re-delivered bubble is not a
        /// reason to fly them home in red.
        case skip = 0
        /// A retraction is already flying; the newest arrival wins the latch.
        case latch = 1
        case adopt = 2
        /// Fly the staged cards home first, then adopt.
        case retract = 3
    }

    /// What to do with a chain that arrived. `sameChain` is the byte comparison
    /// of the arriving payload against the one this board is built on.
    public static func arrival(_ s: State, sameChain: Bool) -> Arrival {
        Arrival(rawValue: fio_msg_turn_arrival(s.rawValue, sameChain ? 1 : 0)) ?? .adopt
    }

    /// The narrower duplicate guard the adopt path keeps for its direct callers:
    /// with moves staged the resident game is base+pending, so a direct adopt
    /// rebuilds rather than skipping.
    public static func adoptIsDuplicate(_ s: State, sameChain: Bool) -> Bool {
        fio_msg_turn_adopt_duplicate(s.rawValue, sameChain ? 1 : 0) != 0
    }

    // MARK: - what a send means

    public enum SentSource: Int32, Sendable {
        case none = 0
        case host = 1
        case sealed = 2
    }

    /// WHICH BYTES WENT OUT, as a choice between two blobs the kernel never
    /// sees. With moves staged our own sealed chain is the bubble - there is
    /// exactly one in the input field and this device sealed it, so the signal
    /// can only be that bubble going out whatever bytes rode along. With nothing
    /// staged only the host can say.
    public static func sentSource(staged: Bool, host: Bool, sealed: Bool) -> SentSource {
        SentSource(rawValue: fio_msg_turn_sent_source(staged ? 1 : 0, host ? 1 : 0,
                                                      sealed ? 1 : 0)) ?? .none
    }

    public enum SendVerdict: Int32, Sendable {
        /// Not our bytes: keep the base AND the staged moves - they are what the
        /// board is drawn from.
        case foreign = 0
        /// Nothing staged and no bytes: there was no send of ours here.
        case noop = 1
        /// Staged, with no chain to rebase onto. Keep the moves.
        case blind = 2
        /// These are ours to adopt - decode them and ask again.
        case decode = 3
        /// They will not decode: keep the board on its staged move.
        case unreadable = 4
        case rebase = 5
    }

    /// What the send does to this board. Ask once with `decoded` nil; a
    /// `.decode` answer means decode the bytes and ask again, and every other
    /// answer is final.
    ///
    /// EVERY ANSWER RELEASES A HELD SETTLEMENT. Send is the only releaser there
    /// is, so a refusal is a refusal to REBASE and never to release - a hold
    /// kept past the send is a board no tap can move and no arrival can unstick.
    public static func sendVerdict(staged: Bool, host: Bool, sealed: Bool,
                                   hostIsSealed: Bool, decoded: Bool?) -> SendVerdict {
        let d: Int32 = decoded.map { $0 ? 1 : 0 } ?? -1
        return SendVerdict(rawValue: fio_msg_turn_send_verdict(
            staged ? 1 : 0, host ? 1 : 0, sealed ? 1 : 0, hostIsSealed ? 1 : 0, d)) ?? .noop
    }

    // MARK: - what is withheld

    /// The index of the step whose committed board a held settlement shows, or
    /// nil when this turn has nothing to hold. `cut` is the kernel's own
    /// settlement boundary over the same frames (`EvWire.settlementCut`).
    public static func holdState(events: Int, cut: Int?) -> Int? {
        let i = fio_msg_turn_hold_state(Int32(events), Int32(cut ?? -1))
        return i >= 0 ? Int(i) : nil
    }

    // MARK: - what a read publishes

    /// A board publishes a snapshot, sometimes a doctored one.
    public struct Published: Sendable, Equatable {
        /// Publish the withheld snapshot instead of the live board.
        public let showHeldView: Bool
        /// Publish an EMPTY menu. The other half of the hold, and the half that
        /// stops the player acting on a deal they have not been shown.
        public let emptyMenu: Bool
        /// Where the animation now on screen starts.
        public let animAtomsBefore: Int
        /// Hide the cards this open will move, before the first paint.
        public let raiseVeil: Bool
    }

    /// `viewWouldChange` is the host's comparison of the board about to go up
    /// against the one already up - the only input the kernel cannot take
    /// itself, because both views are the host's own decodes. A veil nothing
    /// will take down must never go up.
    public static func publish(_ s: State, baseAtomsBefore: Int, stagedAtomsBefore: Int,
                               openReplay: Int, viewWouldChange: Bool) -> Published {
        var held: Int32 = 0, empty: Int32 = 0, atoms: Int32 = 0, veil: Int32 = 0
        fio_msg_turn_publish(s.rawValue, Int32(baseAtomsBefore), Int32(stagedAtomsBefore),
                             Int32(openReplay), viewWouldChange ? 1 : 0,
                             &held, &empty, &atoms, &veil)
        return Published(showHeldView: held != 0, emptyMenu: empty != 0,
                         animAtomsBefore: Int(atoms), raiseVeil: veil != 0)
    }
}
