// EvWire.swift — decode the kernel's packed evwire animation frame into
// [GameEvent], in Swift, with NO JSON (§zero-JSON). The Swift twin of the web's
// decodeEventWire (sdk/ts/wire/evwire.ts): the kernel derives every animation
// event exactly ONCE (evwire_walk, c/src/evwire.c) and both clients only READ
// the bytes - neither re-derives "which card flew where". This is what lets the
// iMessage board animate a reopened bubble off the kernel's own viewer-aware
// stream (fio_replay_last_events_packed) instead of diffing two GameViews. A
// diff can never recover the viewer's OWN drawn card (the replayed hand is the
// same before and after from the diff's side), which is exactly why the "my own
// refill never animated on reopen" bug existed; the kernel, replaying with the
// viewer's seat, reveals it.
//
// Frame layout (c/src/evwire.h):
//   u8 version, u8 viewer, u8 actor, u8 n_events
//   per event: u8 type, u8 seat, u8 msg, u8 from, u8 to, u8 flags,
//              u8 n_cards, n_cards x u8 wire-card,
//              [u8 target] if flags&1, [u8 battle] if flags&2,
//              u16 snap_len LE, snap_len bytes (viewer-masked put_state)
//   trailer:   u16 final_len LE, final_len bytes (the final committed state)
// A card byte: 0..51 = suit*13 + (value-1); 0xFE hidden; 0xFF none.

import Foundation

public enum EvWire {
    private static let WIRE_HIDDEN: UInt8 = 0xFE
    private static let WIRE_NONE: UInt8 = 0xFF

    /// A card in an event's list, or nil for a REDACTED one (a card back) - the
    /// kernel masks DEAL/REFILL cards to 0xFE for any viewer that is not the
    /// drawing seat. GameEvent.cards documents nil as exactly this.
    private static func card(_ b: UInt8) -> Card? {
        if b == WIRE_NONE || b == WIRE_HIDDEN { return nil }
        let v = Int(b)
        return Card(s: v / 13, v: (v % 13) + 1)
    }

    /// Decode one packed evwire frame (fio_replay_last_events_packed's output, or
    /// a single replay_steps_frames_v6 frame) into its events, each already
    /// masked for the frame's own viewer. Empty on a short/empty/malformed
    /// buffer - a corrupt frame degrades to "no animation", never a crash, the
    /// same discipline the rest of the wire readers keep.
    public static func decode(_ bytes: Data) -> [GameEvent] {
        let b = [UInt8](bytes)
        var p = 0
        func u8() -> Int? { guard p < b.count else { return nil }; defer { p += 1 }; return Int(b[p]) }
        func u16() -> Int? {
            guard p + 1 < b.count else { return nil }
            defer { p += 2 }; return Int(b[p]) | (Int(b[p + 1]) << 8)
        }

        guard let version = u8(), version == EVWIRE_VERSION,
              let viewer = u8(), let _actor = u8(), let n = u8() else { return [] }
        _ = _actor   // the good-players insertion order; the board does not need it

        var events: [GameEvent] = []
        events.reserveCapacity(n)
        for _ in 0..<n {
            guard let type = u8(), let seatByte = u8(), let msg = u8(),
                  let from = u8(), let to = u8(), let flags = u8(), let nCards = u8()
            else { return events }

            var cards: [Card?] = []
            cards.reserveCapacity(nCards)
            for _ in 0..<nCards {
                guard let cb = u8() else { return events }
                cards.append(card(UInt8(cb)))
            }

            var target: Card?
            if flags & 1 != 0 { guard let tb = u8() else { return events }; target = card(UInt8(tb)) }
            var battle: Int?
            if flags & 2 != 0 { guard let bb = u8() else { return events }; battle = bb }

            // Each event carries the viewer-masked board AS OF this step (view.c
            // put_state), decoded with the same MaskedView reader residentView
            // uses - so a multi-event step (a pickup's PICKUP + refill draws +
            // defender change) can settle counts step by step, never a jump.
            guard let snapLen = u16() else { return events }
            var snap: GameView?
            if snapLen > 0, p + snapLen <= b.count {
                snap = MaskedView.decode(Data(b[p..<p + snapLen]), viewer: viewer)
            }
            p += snapLen

            events.append(GameEvent(type: type, seat: seatByte == 0xFF ? -1 : seatByte,
                                    msg: msg, from: from, to: to, cards: cards,
                                    target: target, battle: battle, state: snap))
        }
        return events
    }

    /// c/src/evwire.h EVWIRE_FORMAT_VERSION.
    private static let EVWIRE_VERSION = 1
}
