// roleMotion.ts - HOW a role mark moves: the three gestures, their durations,
// and the arc a mark thrown across the table travels on.
//
// THIS IS A PORT, NOT A DESIGN. Every number and every curve below is copied
// from ios/FoolishKit/Boards/FRoleMotion.swift, which is the shipped iMessage
// board's answer after the owner's rounds 16, 20, 21 and 30. The Swift file
// carries the reasoning; this file carries the same arithmetic so the web board
// and the iMessage board are one object seen twice, and it repeats only as much
// of that reasoning as a reader needs to avoid "improving" a number. Where the
// two could drift, FRoleMotion.swift wins.
//
// THREE MOTIONS, and each says a different thing (FRoleMotion.swift's header):
//
//   THE COIN FLIP - one mark BECOMES another at the same seat. Its width
//   collapses to nothing, the face is swapped at the invisible frame, and it
//   opens back out: an object turning over, not two objects swapped. A
//   cross-fade is two things dissolving; a flip is one thing with two faces.
//
//   THE HALF FLIP - a role BEGINS or ENDS where it stands. Same coin, one half
//   of it. Round 20 replaced a cross-fade here, which is what makes every
//   gesture in this file the same gesture.
//
//   THE FLIGHT - a role LEAVES one seat for another. Only two marks ever do:
//   the defender's shield and the opener's sword. Everything else is a gesture
//   in place, which is the owner's "most swords can fade".
//
// WHAT IS NOT HERE: which mark a seat wears and which marks travel, both of
// which are src/state/roleLedger.ts's; and WHEN a gesture is fired, which is
// the kernel's (anim_goods_opening / anim_goods_cleared / anim_pass_hand_off,
// reached through sdk/ts/wasm/bots.ts). This file is arithmetic only.

import { ANIM_TIME_MS } from '@sdk/ts/wasm/bots.ts';
import type { RoleMarkKind } from '../components/RoleMark';

/** A point on screen, in viewport coordinates - the same space
 *  AnimationOverlay's `centreOf` answers in. */
export interface Spot { x: number; y: number }

// ---- the durations ----------------------------------------------------------
//
// ALL FOUR DERIVE FROM THE KERNEL'S FLIGHT TIME, exactly as iMessage derives
// them from `flightTime` (= ANIM_TIME_MS/1000, BoardFlight.swift:21). A role
// duration is a proportion of a card's, never a number of its own, so slowing
// the board down keeps the proportions instead of leaving the marks behind.

/** A little under a card flight. The role hand-off is the closing beat of a
 *  bout - the cards have already been swept and dealt - so it wants to feel
 *  quick and deliberate rather than ceremonial. `roleFlightTime`. */
export const ROLE_FLIGHT_MS = ANIM_TIME_MS * 0.8;

/** Half a coin flip: collapse, swap, open. Two of these back to back is a full
 *  turn, one on its own is a mark arriving or leaving, and the pair comes in
 *  under a card's motion - the mark is a caption on the move, not the move.
 *  `roleFlipHalf`. */
export const ROLE_FLIP_HALF_MS = ANIM_TIME_MS * 0.22;

/** ROUND 30, the owner: "for the sword -> check rotation, the width doesn't
 *  QUITE go to zero during rotate out before swapping to the other glyph and
 *  rotating in." The collapse is given a deadline slightly BEFORE the swap, so
 *  the difference is a genuine edge-on beat; taken out of the collapse rather
 *  than added to the flip, so the gesture's total length - tuned against the
 *  card flights it captions - does not change at all. `roleFlipSettle`. */
export const ROLE_FLIP_SETTLE_MS = Math.min(30, ROLE_FLIP_HALF_MS * 0.3);

/** How long the collapse itself is given: a settle short of the swap.
 *  `roleFlipCollapse`. */
export const ROLE_FLIP_COLLAPSE_MS = Math.max(10, ROLE_FLIP_HALF_MS - ROLE_FLIP_SETTLE_MS);

/** How long a seat expecting an arrival waits before turning its own mark away,
 *  so the collapse FINISHES as the ghost touches down (the owner: "the shield
 *  flies onto their sword"). `roleMakeWayDelay`. */
export const ROLE_MAKE_WAY_DELAY_MS = Math.max(0, ROLE_FLIGHT_MS - ROLE_FLIP_HALF_MS);

/** How far into the shield's flight a pass's PREVIOUS defender starts turning
 *  its sword in. The ghost is scaled up and right over the seat at take-off; a
 *  third of the way along its arc it has cleared the badge row at every seat
 *  distance. `RoleCoinMotion.passSwordDelay`. */
export const ROLE_PASS_SWORD_DELAY_MS = ROLE_FLIGHT_MS / 3;

/** A ROLE GESTURE'S OWN CLOCK only moves on frames that DREW it, and a gap
 *  between two drawn frames is worth at most this - ONE frame at 60Hz. The
 *  owner, measuring an eight-seat Undo where the sword went 35 -> 10 -> 0 in two
 *  frames: "NO JUMPS IN ROTATION!" A slow board stretches the turn instead of
 *  eating it. `RoleCoinClock.cap`.
 *
 *  THE FLIGHT IS ON THE SAME CLOCK, which iMessage does not need to say: there
 *  `roleProgress` is a SwiftUI `withAnimation`, and CoreAnimation runs it off
 *  the main thread, so a main-thread stall cannot eat it. On the web the same
 *  tween is requestAnimationFrame, which a stall eats whole - measured in
 *  Chromium against a production build, where a ~3.4s stall early in the page's
 *  life (present on main too, and not this change's) swallowed an entire shield
 *  hand-off: one frame of ghost at the take-off pad and then the roles settled.
 *  Capping the flight's step is what CoreAnimation gives iMessage for free. */
export const ROLE_FRAME_CAP_MS = 1000 / 60;

/** The width a coin edge-on is drawn at. Never a true zero: a scale of 0
 *  collapses the box some browsers then decline to paint at all, and the mark
 *  has to come back from it. */
export const ROLE_EDGE_ON = 0.001;

// ---- one gesture of a coin ---------------------------------------------------

/** ONE GESTURE OF A COIN, as a function of the time since it was first drawn.
 *  `from` turns away (collapse, then a beat edge-on - round 30's settle), `to`
 *  comes round; either may be null (a half flip), and `delayMs` holds the old
 *  face first (a seat making way for a mark in flight, a pass's sword waiting
 *  for the shield to leave). `RoleCoinPhase`. */
export interface RoleCoinPhase {
    /** Claims the gesture: a mark can change again mid-flip, and the older
     *  gesture must not finish INTO the face the newer one already swapped
     *  past. `FRoleCoin.gesture`. */
    id: number;
    from: RoleMarkKind | null;
    to: RoleMarkKind | null;
    delayMs: number;
}

/** How long this gesture runs in full. `RoleCoinPhase.total`. */
export const coinTotalMs = (p: RoleCoinPhase): number =>
    p.delayMs + (p.from !== null ? ROLE_FLIP_HALF_MS : 0) + (p.to !== null ? ROLE_FLIP_HALF_MS : 0);

/** THE FACE AND THE WIDTH on the frame being drawn now.
 *
 *  The width is the COSINE of an angle swept at a steady rate, which is what a
 *  coin turning does: widest face-on, fastest through edge-on, and it opens
 *  back out the same way. The owner, round 30: "it should be like a cosine wave
 *  shape, right?" The curve this replaced eased IN, which spent the first half
 *  of the collapse near full width and the rest of it in two frames - the
 *  owner's "the swords basically just blinked out of existence".
 *  `RoleCoinPhase.frame(at:)`. */
export function coinFrameAt(p: RoleCoinPhase, elapsedMs: number): { face: RoleMarkKind | null; scale: number } {
    let t = elapsedMs - p.delayMs;
    if (t < 0) return { face: p.from, scale: 1 };
    if (p.from !== null) {
        if (t < ROLE_FLIP_COLLAPSE_MS) {
            const u = t / ROLE_FLIP_COLLAPSE_MS;
            return { face: p.from, scale: Math.max(ROLE_EDGE_ON, Math.cos((u * Math.PI) / 2)) };
        }
        if (t < ROLE_FLIP_HALF_MS) return { face: p.from, scale: ROLE_EDGE_ON };   // edge-on
        t -= ROLE_FLIP_HALF_MS;
    }
    if (p.to === null) return { face: null, scale: 1 };
    if (t < ROLE_FLIP_HALF_MS) {
        const u = t / ROLE_FLIP_HALF_MS;
        return { face: p.to, scale: Math.max(ROLE_EDGE_ON, Math.sin((u * Math.PI) / 2)) };
    }
    return { face: p.to, scale: 1 };
}

// ---- which gesture --------------------------------------------------------

/** WHICH MOTION a mark makes when it changes, as a value - the rule on its own,
 *  away from the state that plays it. `RoleGesture`. */
export type RoleGesture = 'flip' | 'rotateOut' | 'rotateIn' | 'restore' | 'none';

/** A mark that TRAVELS is not a gesture a seat makes at all - the board blanks
 *  the seat it left and the ghost carries it - so a flight never reaches here.
 *  `RoleGesture.between`. */
export function gestureBetween(shown: RoleMarkKind | null, next: RoleMarkKind | null): RoleGesture {
    if (shown !== null && next !== null) return shown === next ? 'none' : 'flip';
    if (shown !== null) return 'rotateOut';
    if (next !== null) return 'rotateIn';
    return 'none';
}

/** WHICH gesture to make, including the case `gestureBetween` cannot see.
 *
 *  A mark can go away and come straight BACK to what it was within one paint: a
 *  bout end empties the live table before the sweep grid stands up in its place,
 *  and for that single frame every attacker's sword has no reason to exist.
 *  Asking `gestureBetween` alone answers 'none' both times - so the exit started
 *  by the first change is never taken back, and the seat is blanked though it
 *  never stopped wearing its mark.
 *
 *  `settled` is "this seat is at rest showing `shown`". Same mark and settled is
 *  genuinely nothing to do; same mark and UNsettled is the blink, and the answer
 *  is to put it back. `RoleGesture.resolve`. */
export function resolveGesture(shown: RoleMarkKind | null, next: RoleMarkKind | null,
                               settled: boolean): RoleGesture {
    if (shown === next) return settled ? 'none' : 'restore';
    return gestureBetween(shown, next);
}

// ---- the flight -------------------------------------------------------------

/** A role mark in flight between two seats, in viewport coordinates.
 *  `RoleFlight`. */
export interface RoleFlight {
    id: string;
    kind: RoleMarkKind;
    from: Spot;
    to: Spot;
    /** The seat it LEFT (blanks instantly - the ghost is that mark now) and the
     *  seat it is GOING TO (turns its own mark away as the ghost arrives). */
    fromSeat: number;
    toSeat: number;
    /** Total degrees turned over the flight, and in practice a WHOLE number of
     *  turns. It reads as "this was thrown to somebody", which is what a
     *  hand-off is - but the reason it cannot be a fraction of a turn is
     *  mechanical: the ghost is taken away the instant it lands and the seat
     *  draws the real mark upright, so any final angle other than a multiple of
     *  360 snaps on the hand-over. The shield used to lean 24 degrees and did
     *  exactly that (the owner, round 21: "the shield kinda turns a little bit
     *  then turns back"). */
    spin: number;
}

/** The bow in the path. A mark thrown across a table travels over it, not
 *  through it, so the arc lifts toward the top of the board - scaled to the
 *  distance so neighbouring seats get a hop and opposite seats get a sail, and
 *  capped so it can never leave the board. `RoleFlight.control`. */
export function flightControl(f: RoleFlight): Spot {
    const dx = f.to.x - f.from.x;
    const dy = f.to.y - f.from.y;
    const dist = Math.sqrt(dx * dx + dy * dy);
    return {
        x: (f.from.x + f.to.x) / 2,
        y: (f.from.y + f.to.y) / 2 - Math.min(56, dist * 0.28),
    };
}

/** Where the ghost is at `p` (0..1): a quadratic bezier through `control`.
 *  `RoleFlight.point(at:)`. */
export function flightPointAt(f: RoleFlight, p: number): Spot {
    const q = 1 - p;
    const c = flightControl(f);
    return {
        x: q * q * f.from.x + 2 * q * p * c.x + p * p * f.to.x,
        y: q * q * f.from.y + 2 * q * p * c.y + p * p * f.to.y,
    };
}

/** HOW FAR OFF THE TABLE the ghost is at `p`: 1 at the middle of the throw, 0
 *  at both pads. The mark comes off the table toward the viewer and settles back
 *  down onto the badge it lands on, like a card's flight - so the scale and the
 *  shadow are both read off this one number. `RoleFlightsLayer.body`. */
export const flightLift = (p: number): number => 1 - Math.abs(p - 0.5) * 2;

/** The ghost's scale at `p`. */
export const flightScale = (p: number): number => 1 + 0.45 * flightLift(p);

/** The flight's timing curve, the same four numbers every card flight on this
 *  board uses (FlightCard.EASE, iMessage's `.timingCurve(0.25, 0.46, 0.45,
 *  0.94)`), as a function - a ghost on a bezier path cannot be handed to a CSS
 *  transition, so the host eases the progress itself.
 *
 *  Newton on the x-polynomial, then the y-polynomial: the standard inversion,
 *  and 8 iterations is well inside a float's patience at these control points. */
export function roleFlightEase(t: number): number {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const [x1, y1, x2, y2] = [0.25, 0.46, 0.45, 0.94];
    const bez = (a: number, b: number, u: number) =>
        3 * a * u * (1 - u) * (1 - u) + 3 * b * u * u * (1 - u) + u * u * u;
    const slope = (a: number, b: number, u: number) =>
        3 * a * (1 - 4 * u + 3 * u * u) + 3 * b * (2 * u - 3 * u * u) + 3 * u * u;
    let u = t;
    for (let i = 0; i < 8; i++) {
        const d = slope(x1, x2, u);
        if (Math.abs(d) < 1e-6) break;
        u -= (bez(x1, x2, u) - t) / d;
        u = Math.min(1, Math.max(0, u));
    }
    return bez(y1, y2, u);
}

/** One crossed paint before a flight's tween starts, so the ghost renders where
 *  the mark actually is before it begins to move. The same beat, and the same
 *  reason, as a card's (FlightCard.FLIGHT_ARM_MS) and as iMessage's 25ms sleep
 *  in `runRoleFlights`. */
export const ROLE_FLIGHT_ARM_MS = 25;
