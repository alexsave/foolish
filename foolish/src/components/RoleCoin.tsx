// RoleCoin - a seat's role mark, with the motion between one mark and the next.
//
// THIS IS A PORT, NOT A DESIGN. It is `FRoleCoin` from
// ios/FoolishKit/Boards/FRoleMotion.swift, gesture for gesture, and the
// arithmetic it runs on is src/state/roleMotion.ts. The Swift file carries the
// reasoning; this file carries the same behaviour and repeats only as much of
// that reasoning as a reader needs to avoid "improving" a branch.
//
// It holds its OWN displayed mark rather than rendering `kind` directly, because
// a flip has an invisible frame in the middle where the two faces swap - the
// view has to be showing the old mark for the first half and the new one for the
// second, which a stateless component cannot do.
//
// A GESTURE'S CLOCK IS ADVANCED BY THE FRAMES THAT DRAW IT, never by the wall
// clock. iMessage learned this filming a pass and an eight-seat Undo: a board
// that is slow to draw after a heavy update swallows a 110ms gesture whole, so
// what reaches the screen is its end. The owner, measuring one: "It definitely
// does not go to width zero", and "NO JUMPS IN ROTATION!". requestAnimationFrame
// is the same clock SwiftUI's `TimelineView(.animation)` is, and a gap between
// two drawn frames is worth at most one frame at 60Hz
// (ROLE_FRAME_CAP_MS) - so a slow board stretches the turn instead of
// eating it.

import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { RoleMarkSize, RoleMarkView, type RoleMarkKind } from './RoleMark';
import {
    ROLE_FRAME_CAP_MS, ROLE_MAKE_WAY_DELAY_MS, ROLE_PASS_SWORD_DELAY_MS,
    coinFrameAt, coinTotalMs, resolveGesture, type RoleCoinPhase,
} from '../state/roleMotion';

/** The three facts a coin reacts to, read TOGETHER: which gesture to make
 *  depends on which of them moved, and the board sets the roles and the flying
 *  seats in one update, so a per-property reaction would see half a hand-off.
 *  `FRoleCoin.Input`. */
interface Input {
    kind: RoleMarkKind | null;
    /** This seat's mark is IN THE AIR as a flight ghost. Draw nothing where it
     *  stood, and do not animate it away - the ghost already IS that motion, and
     *  a rotate-out here would show the mark leaving twice. */
    departing: boolean;
    /** A mark is flying TO this seat. Whatever is worn here turns away in the
     *  last moments of the flight, and the arriving mark is stood up the instant
     *  the ghost lands. */
    arriving: boolean;
}

/** What the seat should be WEARING for this input: nothing while a mark is on
 *  its way here, since the ghost is that mark until it lands. */
const targetOf = (i: Input): RoleMarkKind | null => (i.arriving ? null : i.kind);

const reduceMotion = (): boolean => {
    try {
        return typeof window !== 'undefined' && !!window.matchMedia
            && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    } catch { return false; }
};

export interface RoleCoinProps {
    seat: number;
    kind: RoleMarkKind | null;
    departing?: boolean;
    arriving?: boolean;
    /** The accessible name of each mark, so a turning coin still says what it is. */
    labelOf: (kind: RoleMarkKind) => string;
    /** This seat's landing pad: the constant box below, published so a shield
     *  can fly from an opponent to me and back. */
    publishPad: (seat: number, el: HTMLElement | null) => void;
}

export const RoleCoin = ({ seat, kind, departing = false, arriving = false, labelOf, publishPad }: RoleCoinProps) => {
    const seed: Input = { kind, departing, arriving };
    // The face at REST. Seeded at first render and not in an effect: this
    // component is also rendered into snapshots that never run effects, and a
    // mark that waited for one would be missing from every snapshot.
    const shownRef = useRef<RoleMarkKind | null>(seed.departing ? null : targetOf(seed));
    const lastRef = useRef<Input>(seed);
    const gestureRef = useRef(0);
    const phaseRef = useRef<RoleCoinPhase | null>(null);
    // The width on the frame being drawn now. A ref AND the DOM: the frame loop
    // writes the element directly (no re-render at 60Hz), and a re-render for
    // some other reason reads it back so the coin is never snapped to full width
    // in the middle of a turn.
    const scaleRef = useRef(1);
    const boxRef = useRef<HTMLDivElement | null>(null);

    const [face, setFace] = useState<RoleMarkKind | null>(shownRef.current);
    const [phase, setPhase] = useState<RoleCoinPhase | null>(null);

    /** Start a gesture on the frame clock. `FRoleCoin.play`. */
    const play = (from: RoleMarkKind | null, to: RoleMarkKind | null, delayMs = 0) => {
        gestureRef.current += 1;
        const p: RoleCoinPhase = { id: gestureRef.current, from, to, delayMs };
        phaseRef.current = p;
        scaleRef.current = coinFrameAt(p, 0).scale;
        setPhase(p);
        setFace(coinFrameAt(p, 0).face);
    };

    const stand = (mark: RoleMarkKind | null) => {
        gestureRef.current += 1;
        phaseRef.current = null;
        shownRef.current = mark;
        scaleRef.current = 1;
        setPhase(null);
        setFace(mark);
    };

    // ---- the rules, on the frame clock (`FRoleCoin.advanceOnFrames`) ----------
    const advance = (now: Input) => {
        const was = lastRef.current;
        lastRef.current = now;

        if (reduceMotion()) return stand(now.departing ? null : targetOf(now));

        // THE GHOST TOOK IT: blank at once, never animated - the mark this seat
        // was wearing is now the one sailing across the table, and easing it away
        // here would be the same object leaving twice at two different speeds. A
        // pass's PREVIOUS defender then turns a sword in behind the departing
        // shield, which the owner asked for as one beat: the shield flies away
        // and the sword rotates in, and they read as cause and effect.
        if (now.departing && !was.departing) {
            const t = targetOf(now);
            stand(null);
            if (t !== null) play(null, t, ROLE_PASS_SWORD_DELAY_MS);
            return;
        }
        // THE GHOST LANDED: the real mark stands up in the same frame the ghost
        // is taken away in. The flight layer is what the eye is following, so the
        // hand-over must be a swap, not a second animation.
        if (was.arriving && !now.arriving) {
            stand(now.departing ? null : targetOf(now));
            return;
        }
        // SOMETHING IS ON ITS WAY HERE: turn away what is worn, timed so the
        // collapse finishes as the ghost touches down - the owner's "they should
        // rotate out AND the sword will land on them".
        //
        // THE MARK IT IS WEARING, not the one a half-played gesture was heading
        // to. The roles publish a beat before the hand-off is planned, so this
        // seat has usually just STARTED a flip to the very mark that is now
        // flying to it - and turning that away would first stand it up. The
        // owner, watching a pass: "at one point there are two shields. Wtf" - and
        // "there should NEVER be more than one shield". So a flip heading for the
        // arriving mark is taken back to the face it came from.
        if (now.arriving && !was.arriving) {
            const p = phaseRef.current;
            const wearing = p ? (p.to === now.kind ? p.from : p.to) : shownRef.current;
            stand(wearing);
            if (wearing !== null) play(wearing, null, ROLE_MAKE_WAY_DELAY_MS);
            return;
        }
        // Nothing is flying: the ordinary gesture between two marks.
        const resting = phaseRef.current?.from ?? shownRef.current;
        switch (resolveGesture(resting, targetOf(now), phaseRef.current === null)) {
            case 'none':
                return;
            case 'restore':
                // Never animated: the mark was already there and nothing about it
                // changed - the only thing that moved was a gesture that turned
                // out to be wrong. Easing it back would be a motion nobody asked
                // for.
                return stand(targetOf(now));
            default:
                shownRef.current = resting;
                return play(resting, targetOf(now));
        }
    };

    // The three facts are read TOGETHER, in a LAYOUT effect: it runs after the
    // DOM is written and before the browser paints, so the gesture starts in the
    // very frame the hand-off is published in - an ordinary effect would fire
    // after a paint the coin should already have been turning through. A
    // render-phase update would be the same beat again, but this component
    // mutates refs as it decides and React may render it twice for its own
    // reasons, which would make that decision twice.
    useLayoutEffect(() => {
        const was = lastRef.current;
        if (kind === was.kind && departing === was.departing && arriving === was.arriving) return;
        advance({ kind, departing, arriving });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [kind, departing, arriving]);

    // ---- the gesture's own clock ---------------------------------------------
    useEffect(() => {
        if (!phase) return undefined;
        let handle = 0;
        let last: number | null = null;
        let elapsed = 0;
        const total = coinTotalMs(phase);
        const step = (t: number) => {
            if (last !== null) elapsed += Math.min(Math.max(0, t - last), ROLE_FRAME_CAP_MS);
            last = t;
            const f = coinFrameAt(phase, elapsed);
            scaleRef.current = f.scale;
            if (boxRef.current) boxRef.current.style.transform = `scaleX(${f.scale})`;
            setFace((prev) => (prev === f.face ? prev : f.face));
            if (elapsed >= total) {
                // Stand the gesture's last face up once it has played IN FULL,
                // counted from the frame that first drew it - never from when it
                // was asked for.
                if (phaseRef.current?.id === phase.id) {
                    phaseRef.current = null;
                    shownRef.current = phase.to;
                    scaleRef.current = 1;
                    if (boxRef.current) boxRef.current.style.transform = 'scaleX(1)';
                    setPhase(null);
                    setFace(phase.to);
                }
                return;
            }
            handle = requestAnimationFrame(step);
        };
        handle = requestAnimationFrame(step);
        return () => cancelAnimationFrame(handle);
    }, [phase]);

    // A CONSTANT BOX, always present, whether or not this seat wears a mark: the
    // row then has nothing to re-lay-out when a mark arrives or leaves (the name
    // below it must not twitch), and - the reason it matters here - it always has
    // a frame to publish, so a flight can take off from a seat that is not
    // currently wearing anything.
    // A STABLE ref callback. An inline one is a new function every render, which
    // React answers by detaching (null) and re-attaching the node each time - and
    // each detach takes this seat's landing pad off the board for the length of a
    // commit.
    const mount = useCallback((el: HTMLDivElement | null) => {
        boxRef.current = el;
        publishPad(seat, el);
    }, [seat, publishPad]);

    return (
        <div
            ref={mount}
            data-role-seat={seat}
            data-role-mark={face ?? ''}
            style={{
                height: `${RoleMarkSize.rowHeight}px`,
                width: `${RoleMarkSize.rowHeight}px`,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                // The turn is a WIDTH scale about the centre, not a cross-fade:
                // a cross-fade is two things dissolving, a flip is one thing with
                // two faces.
                transform: `scaleX(${scaleRef.current})`,
                willChange: 'transform',
            }}
        >
            {face && <RoleMarkView kind={face} label={labelOf(face)} />}
        </div>
    );
};
