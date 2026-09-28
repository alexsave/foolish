// EvWire.swift - the kernel's animation stream, READ BY THE KERNEL.
//
// The kernel derives every animation event exactly ONCE (evwire_walk,
// c/src/evwire.c) and both clients only READ the bytes - neither re-derives
// "which card flew where". This is what lets the iMessage board animate a
// reopened bubble off the kernel's own viewer-aware stream
// (fio_replay_last_events_packed) instead of diffing two GameViews. A diff can
// never recover the viewer's OWN drawn card (the replayed hand is the same
// before and after from the diff's side), which is exactly why the "my own
// refill never animated on reopen" bug existed; the kernel, replaying with the
// viewer's seat, reveals it.
//
// This file used to READ those bytes here: the four header bytes, then each
// event's seven fixed bytes, its cards, its optional target and battle, and the
// u16 snapshot behind them - a second statement of evwire.h's layout, in Swift,
// beside the C that writes it. The kernel has had that reader since Phase 5a
// (client_push_open / next / final, which is what the WEB walks its pushes
// with), so the walk is gone: a frame is opened, stepped and closed through
// fio_push_*, and each step's board comes back as the same TableView every
// other board on this platform comes from.
//
// Even the FRAMING is the kernel's now (fio_evw_frames): the stream is
// length-prefixed sequences, and counting them is a rule rather than a
// convenience - a step emits any number of events, including none (a good that
// does not close the bout), so "how far back does this stream reach" counts
// frames and never events.
//
// A sequence that does not read WHOLE animates nothing, which is stricter than
// the old walk (it kept whatever had decoded before the damage) and is the
// kernel's own discipline: half a sequence rendered as a whole one is worse
// than none. Nothing in production produces one - these bytes are the kernel's
// own output, handed straight back.

import Foundation
import CFoolish

public enum EvWire {

    /// Decode a run of LENGTH-PREFIXED frames (replay_steps_frames_v6's output
    /// shape, which is what fio_replay_last_events_packed hands back) into one
    /// flat event stream, in play order.
    ///
    /// A turn is several frames because it is several ACTIONS: an iMessage
    /// bubble carries everything its sender staged, so a defender who covered
    /// twice sends two cover steps and both have to be replayed. Flattening
    /// here rather than in the board keeps the caller's contract unchanged - it
    /// still gets "the events of what just happened", in order - and each event
    /// still carries its own per-step board snapshot, so counts settle step by
    /// step across the whole turn exactly as they do within one.
    public static func decodeFrames(_ bytes: Data) -> [GameEvent] {
        var out: [GameEvent] = []
        forEachFrame(bytes) { out.append(contentsOf: decode($0)) }
        return out
    }

    /// How many STEPS the stream holds - its frame count, the kernel's own.
    /// `lastMoveEventsWithPrior` is the caller that needs it: a step with no
    /// events is still a step, so nothing may count events for this.
    public static func frameCount(_ bytes: Data) -> Int {
        let n: Int32 = bytes.withUnsafeBytes { raw in
            fio_evw_frames(raw.bindMemory(to: UInt8.self).baseAddress, Int32(bytes.count), nil, nil, 0)
        }
        return n > 0 ? Int(n) : 0
    }

    /// THE BOARD THE FIRST STEP COMMITTED - the frame's trailer (evwire.h: the
    /// viewer-masked final state written after the last event).
    ///
    /// Not the same thing as the last event's snapshot, and better for the one
    /// question that asks it: a step with NO events still commits a board, and
    /// that is exactly the case (a bare good) where a client most needs to know
    /// what the table looked like. nil for a stream whose first frame does not
    /// read whole.
    public static func firstFrameFinalState(_ bytes: Data) -> GameView? {
        var out: GameView?
        forEachFrame(bytes) { frame in
            guard out == nil else { return }
            guard open(frame) else { return }
            while fio_push_next() == 1 {}
            if fio_push_final() == Int32(CLIENT_OK) { out = MaskedView.table().map(GameView.init(kernel:)) }
        }
        return out
    }

    /// Decode ONE packed evwire frame into its events, each already masked for
    /// the frame's own viewer, and each carrying the board its step commits.
    /// Empty for a frame that does not read whole.
    public static func decode(_ bytes: Data) -> [GameEvent] {
        guard open(bytes) else { return [] }
        var events: [GameEvent] = []
        while fio_push_next() == 1 {
            guard let ep = fio_push_event_ptr(), let e = try? readPushEvent(ep),
                  let board = MaskedView.table() else { break }
            events.append(GameEvent(type: e.type, seat: e.seat, msg: e.msg, from: e.from, to: e.to,
                                    // nil is a REDACTED card (a back): the kernel
                                    // masks DEAL/REFILL cards for any viewer that
                                    // is not the drawing seat.
                                    cards: e.cards.map { $0.suit < 0 ? nil : Card(s: $0.suit, v: $0.value) },
                                    target: e.hasTarget ? Card(s: e.target.suit, v: e.target.value) : nil,
                                    battle: e.battle >= 0 ? e.battle : nil,
                                    state: GameView(kernel: board)))
        }
        _ = fio_push_final()
        return events
    }

    /// WHERE A TURN SETTLES, over the same frame stream `decodeFrames` reads:
    /// the index, into the flattened event list, of the first step that belongs
    /// to the bout end rather than to the move that caused it. nil when the turn
    /// ended no bout.
    ///
    /// The kernel answers it (fio_evw_frames_settlement_cut). Both the rule -
    /// which step types a bout end owns - and the counting ACROSS frames are
    /// facts about the wire.
    ///
    /// Ask it of the SAME bytes `decodeFrames` was given, in the same breath: the
    /// cut indexes the list those bytes produce and nothing else.
    public static func settlementCut(_ bytes: Data) -> Int? {
        let cut = bytes.withUnsafeBytes { raw -> Int32 in
            fio_evw_frames_settlement_cut(raw.bindMemory(to: UInt8.self).baseAddress,
                                          Int32(bytes.count))
        }
        return cut >= 0 ? Int(cut) : nil
    }

    // MARK: - the walk

    /// Open one frame for stepping. The bytes stay where they are while the
    /// kernel walks them, which is why every caller below does its whole walk
    /// inside the `withUnsafeBytes` that produced them.
    private static func open(_ frame: Data) -> Bool {
        frame.withUnsafeBytes { raw in
            fio_push_open(raw.bindMemory(to: UInt8.self).baseAddress, Int32(frame.count)) == Int32(CLIENT_OK)
        }
    }

    /// Each frame of a length-prefixed stream, in play order. The offsets are
    /// the kernel's (fio_evw_frames); a stream that is not whole yields none.
    private static func forEachFrame(_ bytes: Data, _ body: (Data) -> Void) {
        let count = frameCount(bytes)
        guard count > 0 else { return }
        var off = [Int32](repeating: 0, count: count)
        var len = [Int32](repeating: 0, count: count)
        let n: Int32 = bytes.withUnsafeBytes { raw in
            fio_evw_frames(raw.bindMemory(to: UInt8.self).baseAddress, Int32(bytes.count),
                           &off, &len, Int32(count))
        }
        guard n == Int32(count) else { return }
        for i in 0..<count {
            body(bytes.subdata(in: Int(off[i])..<Int(off[i] + len[i])))
        }
    }
}
