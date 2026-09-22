// useAnimationRun.ts - the web's animation engine, which is one frame loop.
//
// Split out of src/contexts/AnimationContext.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4) once the kernel took the timing, because what was left is a
// self-contained thing with a name: a RUN of steps, a clock, and the four pieces
// of React state a run drives. The provider around it is then about the game -
// the feed, the version gate, the optimistic policy, the four move methods - and
// nothing in it decides when a card moves any more.
//
// WHAT THIS OWNS: the run, its origin, how far React has caught up to the
// kernel's landings, the animation frame it asked for, and the state the page
// renders from (what is flying, for how long, the deck's in-flight counts, and
// which cards are hidden where).
//
// WHAT IT DOES NOT: what a landing MEANS. A landed step commits a board, may
// release some tracking, may count a sequence down - all of that is the
// provider's, handed over as `onLanded` and `onIdle`, because it is about the
// game and not about time.

import { useCallback, useEffect, useRef, useState } from 'react';
import { ANIM_STEP_NONE, type AnimPlanSnap } from '@sdk/ts/wasm/bots.ts';
import {
    arrivingPiles, frameAt, heldPiles, NO_ARRIVING, NO_HELD, planFor, sameArriving, sameHeld,
    type AnimStep, type ArrivingPile,
} from './animPlan';
import type { TableView } from './view';

/** What the page draws for one card at one place while its flight is up. */
export interface CardFlight {
    animationType: string;
    progress: number;
    fromLocation: string | null;
    toLocation: string | null;
    startTime: number;
}

/** A step the loop plays: the plan's fields, plus what its landing is worth. */
export interface RunStep extends AnimStep {
    /** The places this flight's card is drawn at, by the page's own owner keys. */
    places?: (number | string)[];
}

export interface AnimationRunHooks<S extends RunStep> {
    /** The board on screen right now: the plan's boardless fallback reads it. */
    board: () => TableView | undefined;
    /** One step has landed. The provider decides what that is worth. */
    onLanded: (step: S) => void;
    /** Every step has landed and the run is over. */
    onIdle: () => void;
    /** Steps were added to the run. */
    onQueued: (steps: S[]) => void;
    /** The places a step's cards are hidden at while it flies (rendering). */
    placesOf: (step: S) => (number | string)[];
    /** The ONE flight a beat's steps make together, for the beats the kernel
     *  merges (consecutive covers by one seat - the kernel spends one COVER
     *  event per card, and one move must move as one). Rendering only: a
     *  landing is still taken per step, in order, so what a landing MEANS is
     *  untouched by the merge. Never called for a beat of one step. */
    mergeBeat: (steps: S[]) => S;
    /** The key the page names one card at one place by (rendering). */
    keyOf: (card: { suit: number; value: number }, place: number | string) => string;
}

export function useAnimationRun<S extends RunStep>(hooks: AnimationRunHooks<S>) {
    const [isAnimating, setIsAnimating] = useState(false);
    const [currentAnimation, setCurrentAnimation] = useState<S | null>(null);
    const [flightMs, setFlightMs] = useState(0);
    const [rowMs, setRowMs] = useState(0);
    const [heldSet, setHeld] = useState<ReadonlySet<number>>(NO_HELD);
    const [arriving, setArriving] = useState<readonly ArrivingPile[]>(NO_ARRIVING);
    const [inFlightFromDeck, setInFlightFromDeck] = useState(0);
    const [inFlightToFlipped, setInFlightToFlipped] = useState(0);
    const [animatingCards, setAnimatingCards] = useState<Map<string, CardFlight>>(new Map());

    // THE RUN IS APPENDED TO, NEVER SPLICED. A step opens at i x (duration +
    // gap), a pure function of its index, so appending to a run in flight leaves
    // every earlier step's timing exactly where it was. That is what lets an
    // arrival be answered by the NEXT call instead of by editing the chain a
    // timer is walking, which is what the queue this replaced needed four
    // insertion branches for.
    const runRef = useRef<S[]>([]);
    // performance.now() when step 0 opened, or null when nothing is running. A
    // MONOTONIC clock, deliberately: a wall-clock jump mid-flight would land
    // every remaining step of the run at once.
    const originRef = useRef<number | null>(null);
    // How many of the run's steps have had their landing taken. The kernel's
    // AnimFrame.landed is the truth; this is how far React has caught up to it.
    const landedRef = useRef(0);
    // THE PLAN'S DURATION FOR THE STEP THE ROW IS MAKING ROOM FOR. The grid's
    // layout change happens at a step's START now, not at its landing
    // (`arrivingPiles`), so the row and the flight are one movement over one
    // number: the duration of the step whose card is in the air. It is still
    // not `flightMs` at the moment it is read - the row also moves at a landing
    // (a board that got ahead releasing its held pile, a sweep taking cells
    // away), and a landing falls in the gap between two flights where
    // anim_plan_at answers ANIM_STEP_NONE and `flightMs` is already 0. This
    // holds the last step to have OPENED, which is that step either way.
    //
    // iMessage writes the same number at the same moment: the row is advanced
    // "with the plan's own duration on it" (ShownLedger.swift), inside
    // `withAnimation(.timingCurve(..., duration: flightTime))`, in the same
    // breath as the flight and before it (MessageTableView+Sequence.swift).
    // 0 before a run's first step opens and after a seek.
    const rowMsRef = useRef(0);
    // How many of the run's steps have OPENED - the kernel's own layout
    // (AnimPlanStep.start_ms) counted against the run's clock, not a second
    // schedule. A step that has opened has its cards in the air, so its pile
    // has earned its slot; every step behind it has not.
    const startedRef = useRef(0);
    const frameHandleRef = useRef<number | null>(null);
    // The merged step a multi-step beat draws as, held so the page is handed
    // the SAME object every frame: `currentAnimation` is compared by identity
    // (a new object per frame would re-run the overlay's layout effect, which
    // is what re-arms the flight, 60 times a second). Keyed by the beat's span
    // and dropped when the run ends, because a later run's first beat has the
    // same span and different cards.
    const beatRef = useRef<{ key: string; step: S } | null>(null);

    // The hooks are read through a ref so the loop never holds a stale closure:
    // it is armed once per frame and the provider re-renders under it.
    const hooksRef = useRef(hooks);
    hooksRef.current = hooks;

    // The cards a step has in the air, keyed by the place the page draws each of
    // them at. Rendering, and the one part of the veil that is: the kernel's veil
    // is per identity (c/src/anim_plan.h), the page's is per PLACE, because a
    // flight has to be hidden where it left AND where it lands or the card is on
    // screen twice.
    const veilOf = (step: S | null): Map<string, CardFlight> => {
        const veil = new Map<string, CardFlight>();
        if (!step?.cards || step.cards.length === 0) return veil;
        const places = hooksRef.current.placesOf(step);
        for (const card of step.cards) {
            for (const place of places) {
                veil.set(hooksRef.current.keyOf(card, place), {
                    animationType: step.type,
                    progress: 1, // Always 1 - CSS transitions handle the animation
                    fromLocation: step.from_location || null,
                    toLocation: step.to_location || null,
                    startTime: Date.now(),
                });
            }
        }
        return veil;
    };

    // The steps of one beat, as the one step the page draws. A beat of one is
    // that step itself - the common case, and no object is made for it.
    const beatAt = (run: S[], plan: AnimPlanSnap, i: number): S | null => {
        const st = plan.steps[i];
        const first = st?.beatFirst ?? i;
        const group = run.slice(first, first + (st?.beatN ?? 1));
        if (group.length <= 1) return group[0] ?? null;
        const key = `${first}:${group.length}`;
        if (beatRef.current?.key === key) return beatRef.current.step;
        const step = hooksRef.current.mergeBeat(group);
        beatRef.current = { key, step };
        return step;
    };

    const tickRef = useRef<() => void>(() => {});

    // ONE FRAME. Ask the kernel where the run stands, then do what it says.
    //
    // THE PLAN IS REBUILT EVERY FRAME, on purpose. It is a pure function of the
    // run (the same steps give the same plan), the kernel keeps exactly one, and
    // a plan built once and sampled later is a plan some other screen may have
    // replaced. Rebuilding is also what makes an arrival free: the next frame
    // plans the longer run and answers about it.
    //
    // A FRAME THE BROWSER SKIPPED lands every step it skipped, in order, in that
    // one frame - a hidden tab comes back to the board it should be holding
    // rather than replaying the whole sequence one flight at a time.
    const tick = useCallback(() => {
        frameHandleRef.current = null;
        const origin = originRef.current;
        if (origin === null) return;
        const run = runRef.current;
        const plan = planFor(run, hooksRef.current.board());
        const nowMs = performance.now() - origin;
        const frame = frameAt(nowMs);

        while (landedRef.current < frame.landed && landedRef.current < run.length) {
            hooksRef.current.onLanded(run[landedRef.current++]);
        }

        // WHICH STEPS HAVE OPENED. The plan lays every step's `start_ms` out
        // against the same clock this frame was sampled at, so this is the
        // kernel's answer re-read and not a second one: a step whose start has
        // passed has its cards in the air, and the steps of one beat open at
        // the same millisecond and are counted together by that alone.
        while (startedRef.current < run.length
               && (plan.steps[startedRef.current]?.startMs ?? Number.MAX_SAFE_INTEGER) <= nowMs) {
            // THE ROW MOVES WITH THE CARD, so it moves over the card's own
            // duration and it starts when the card does.
            rowMsRef.current = plan.steps[startedRef.current]?.durationMs ?? 0;
            startedRef.current++;
        }
        setRowMs((prev) => (prev === rowMsRef.current ? prev : rowMsRef.current));

        // THE PILES THE GRID MUST NOT MAKE ROOM FOR YET (animPlan.heldPiles) -
        // the steps that have not OPENED. A step whose flight is up has already
        // earned its slot; the grid makes room for it while the card crosses.
        // A SET AND NOT A ROW, deliberately: the grid takes these out of
        // whatever board it is drawing at the instant it draws it, so a board
        // committed between two frames is answered in the commit that carries
        // it rather than one frame later.
        const held = heldPiles(plan.pre, run.slice(startedRef.current));
        setHeld((prev) => (sameHeld(prev, held) ? prev : held));

        // …AND THE PILES IT MUST MAKE ROOM FOR NOW: the steps that have opened
        // and not landed, whose cards are crossing the board at this instant
        // (animPlan.arrivingPiles). The row grows around them as they come
        // down, which is round 7's "it should be at the same TIME" read onto
        // the table (ios/FoolishKit/Boards/MessageTableView+Sequence.swift).
        const coming = arrivingPiles(run.slice(frame.landed, startedRef.current));
        setArriving((prev) => (sameArriving(prev, coming) ? prev : coming));

        // WHAT FLIES IS A BEAT, NOT A STEP, and the kernel says which is which
        // (AnimPlanStep.beat_first / beat_n). The plan opens every step of one
        // beat at the same instant; if the page then drew only run[frame.step]
        // the second card of a two-card cover would never be drawn at all. The
        // grouping and the timing come from the one answer for exactly this
        // reason - a host that merged on a rule of its own could merge
        // somewhere the clock did not.
        const flying = frame.step === ANIM_STEP_NONE ? null : beatAt(run, plan, frame.step);
        setCurrentAnimation((prev) => (prev === flying ? prev : flying));
        setFlightMs(frame.step === ANIM_STEP_NONE ? 0 : plan.steps[frame.step]?.durationMs ?? 0);
        // The stock shrinks as cards LEAVE it, not as they land, and a card bound
        // for the trump's slot never leaves it at all: both numbers are the
        // kernel's, per step (AnimPlanStep.in_flight_from_deck / _to_flipped).
        setInFlightFromDeck(frame.inFlightFromDeck);
        setInFlightToFlipped(frame.inFlightToFlipped);
        setAnimatingCards((prev) => {
            const next = veilOf(flying);
            if (prev.size === next.size && [...next.keys()].every((k) => prev.has(k))) return prev;
            return next;
        });

        if (frame.done && landedRef.current >= run.length) {
            runRef.current = [];
            originRef.current = null;
            landedRef.current = 0;
            beatRef.current = null;
            startedRef.current = 0;
            setCurrentAnimation(null);
            setFlightMs(0);
            setHeld(NO_HELD);
            setArriving(NO_ARRIVING);
            // `rowMs` is NOT cleared here, and that is deliberate: the run's
            // last landing moves the row in this very frame and is entitled to
            // the same glide every earlier one had. The next run zeroes it as
            // it starts, and a seek's `reset` zeroes it outright.
            setIsAnimating(false);
            setInFlightFromDeck(0);
            setInFlightToFlipped(0);
            setAnimatingCards((prev) => (prev.size === 0 ? prev : new Map()));
            hooksRef.current.onIdle();
            return;
        }
        frameHandleRef.current = requestAnimationFrame(() => tickRef.current());
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);
    tickRef.current = tick;

    /**
     * Add steps to the run, starting it if nothing is playing. A step queued
     * while a run is in flight simply extends it; the kernel's plan gives it the
     * next slot and the earlier steps keep the timing they already had.
     */
    const enqueue = useCallback((steps: S[]) => {
        if (steps.length === 0) return;
        hooksRef.current.onQueued(steps);
        runRef.current = [...runRef.current, ...steps];
        if (originRef.current !== null) return;
        originRef.current = performance.now();
        landedRef.current = 0;
        startedRef.current = 0;
        rowMsRef.current = 0;
        setRowMs(0);
        setIsAnimating(true);
        // The first frame is asked for NOW rather than on the next paint: a
        // caller that queued a step and then read `currentAnimation` in the same
        // commit sees the flight it started, exactly as the queue's first
        // setTimeout(0)-equivalent used to give it.
        tick();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    /** Drop everything queued or in flight, committing nothing. */
    const reset = useCallback(() => {
        if (frameHandleRef.current !== null) {
            cancelAnimationFrame(frameHandleRef.current);
            frameHandleRef.current = null;
        }
        runRef.current = [];
        originRef.current = null;
        landedRef.current = 0;
        startedRef.current = 0;
        rowMsRef.current = 0;
        beatRef.current = null;
        setCurrentAnimation(null);
        setFlightMs(0);
        setRowMs(0);
        setHeld(NO_HELD);
        setArriving(NO_ARRIVING);
        setIsAnimating(false);
        setInFlightFromDeck(0);
        setInFlightToFlipped(0);
        setAnimatingCards(new Map());
    }, []);

    useEffect(() => () => {
        if (frameHandleRef.current !== null) {
            cancelAnimationFrame(frameHandleRef.current);
            frameHandleRef.current = null;
        }
    }, []);

    return {
        isAnimating, currentAnimation, flightMs, rowMs, heldPiles: heldSet,
        arrivingPiles: arriving,
        inFlightFromDeck, inFlightToFlipped, animatingCards, enqueue, reset,
    };
}
