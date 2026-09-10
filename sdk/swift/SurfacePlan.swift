// SurfacePlan.swift - how an open lobby takes a chain that just arrived.
//
// THE REPORT, 1.1(56), owner: "LOBBY DID NOT UPDATE LIVE! I was in lobby, got a
// start game text, and it was stuck on lobby! I think it should fade from lobby
// to the game in this case."
//
// ONE BUBBLE CAN CARRY SEVERAL ACTIONS. `conversation.insert` REPLACES an unsent
// draft rather than queueing a second one, so a player who taps Join, then the
// rules checkbox, then Start sends ONE envelope - and a surface that jumps to
// its end state skips the very thing the human was sitting there waiting to see.
// So the arrival is played as a sequence, one beat per action, with a rest
// between them.
//
// NOTHING HERE DECIDES ANYTHING. What actions the stream contains, what order
// they must have happened in, how long each rests, and WHICH IDIOM each wears -
// snap, turn, fade - are all the kernel's (c/src/msg_wire.c's msg_surface_delta
// composed with c/src/anim_plan.c's anim_surface_plan, crossed by
// fio_msg_surface_plan). This file only decodes that answer, and the view only
// renders it: the curve of a fade and the anchor of a turn are the only parts a
// C function genuinely cannot do.
//
// An empty plan is the ordinary case and means "there is nothing to stage" - a
// board taking an arrival, or a change that is simply true now. The caller then
// adopts exactly as it always did, which IS the snap.

import Foundation
import CFoolish

public struct SurfacePlan: Equatable, Sendable {

    /// WHAT an action was.
    public enum Kind: Int, Sendable {
        case roster = 1   // somebody sat down, or the roster changed at once
        case rules  = 2   // the table's rules moved
        case board  = 3   // the game is dealt and the lobby is over
    }

    /// HOW it arrives. Read, never chosen: see the file header.
    public enum Transition: Int, Sendable {
        case snap = 0
        case turn = 1
        case fade = 2
    }

    public struct Beat: Equatable, Sendable {
        public let kind: Kind
        public let transition: Transition
        /// The passing rule this beat shows - the OLD one until the rules beat
        /// moves it. The roster is always the arriving chain's (a message
        /// carries at most one roster action, so there is no intermediate
        /// roster to draw), which is what makes playing the LAST beat the same
        /// thing as adopting the chain.
        public let passing: Bool
        /// This beat's own motion. Zero for a snap.
        public let duration: TimeInterval
        /// When it begins, measured from the arrival. The REST between beats
        /// lives in the gaps between these, so a caller waits for a beat's
        /// start rather than sleeping a number of its own.
        public let start: TimeInterval
    }

    public let beats: [Beat]
    public let total: TimeInterval

    public static let none = SurfacePlan(beats: [], total: 0)

    fileprivate init(beats: [Beat], total: TimeInterval) {
        self.beats = beats
        self.total = total
    }

    /// Decode `fio_msg_surface_plan`'s int32 answer. A shape this does not
    /// recognise is `.none`, which is the same degrade-to-less-animation
    /// discipline every other wire reader here keeps.
    fileprivate init(words: [Int32], count: Int) {
        let head = Int(FIO_SURFACE_HEAD), stride = Int(FIO_SURFACE_STRIDE)
        guard count >= head else { self = .none; return }
        let n = Int(words[0])
        guard n > 0, count >= head + n * stride else { self = .none; return }
        var out: [Beat] = []
        out.reserveCapacity(n)
        for i in 0..<n {
            let w = head + i * stride
            guard let kind = Kind(rawValue: Int(words[w])),
                  let trans = Transition(rawValue: Int(words[w + 1]))
            else { self = .none; return }
            out.append(Beat(kind: kind, transition: trans,
                            passing: words[w + 2] != 0,
                            duration: TimeInterval(words[w + 3]) / 1000,
                            start: TimeInterval(words[w + 4]) / 1000))
        }
        self.beats = out
        self.total = TimeInterval(words[1]) / 1000
    }
}

extension MessageKernel {
    /// What should the surface showing `showing` do about `arriving`?
    ///
    /// HEADER READS ONLY, exactly as `preferred` is: two decodes into the
    /// bridge's own scratch, the resident game untouched. That matters here
    /// because this is asked BEFORE the arrival is adopted - a decode that
    /// adopted would move the game out from under the board being asked about,
    /// which is the phantom-seal shape (see `resealFromBase`).
    public func surfacePlan(showing: Data, arriving: Data) -> SurfacePlan {
        let cap = Int(FIO_SURFACE_HEAD) + 10 * Int(FIO_SURFACE_STRIDE)
        var out = [Int32](repeating: 0, count: cap)
        let n: Int32 = showing.withUnsafeBytes { sp in
            arriving.withUnsafeBytes { ap in
                fio_msg_surface_plan(sp.bindMemory(to: UInt8.self).baseAddress, Int32(showing.count),
                                     ap.bindMemory(to: UInt8.self).baseAddress, Int32(arriving.count),
                                     &out, Int32(cap))
            }
        }
        guard n > 0 else { return .none }
        return SurfacePlan(words: out, count: Int(n))
    }
}
