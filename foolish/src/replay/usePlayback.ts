/* =============================================================================
 * The replay's transport: where the deck head is, and what moves it
 * =============================================================================
 * The scrubber's own state and nothing else - which step is on screen, whether
 * it is playing, and the wall-clock target the next move is armed against. The
 * CHOREOGRAPHY is not here and never was: a step is played by publishing its
 * sequence into the animation feed, and the kernel's plan decides what flies,
 * for how long and in what order (src/state/useAnimationRun.ts).
 *
 * What stays in React, by docs/ANIM_TIMING_AUDIT.md section 1: the speed dial,
 * the clamps over the RECORDED gaps between two moves, and the ticker's
 * interval. They are a transport over wall-clock times a game once took, not
 * choreography, and the kernel has no concept of any of them.
 * ========================================================================== */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useServerActions } from '../contexts/ServerContext';
import { useAnimation } from '../contexts/AnimationContext';
import { animationFeed, AnimationSequenceMessage } from '../state/animationFeed';
import { buildSpeeds, type SpeedStop } from './speeds';
import { REPLAY_KEY, REPLAY_STEP, type ReplayFrame } from './frames';

/** The deck head and the buttons that move it. */
export interface Playback {
    /** The step on screen; -1 until the opening deal has been published. */
    stepIdx: number;
    lastIdx: number;
    /** The frame on screen (the first one while the deal is still pending). */
    frame: ReplayFrame;
    playing: boolean;
    setPlaying: (next: boolean | ((p: boolean) => boolean)) => void;
    /** When the armed move is due, or null when none is armed. */
    waitTarget: number | null;
    /** The ticker's clock, for the countdown readout. */
    now: number;
    speeds: SpeedStop[];
    speedIdx: number;
    nextSpeed: () => void;
    stepForward: () => void;
    stepBack: () => void;
    jumpTo: (i: number) => void;
    nextBout: () => void;
    boutStart: () => void;
}

/** The telestrator, which the keyboard shares: 'c' toggles it, and while it is
 *  on a commentator's keystrokes must not scrub the replay underneath the pen. */
export interface PenKeys {
    drawing: boolean;
    toggleDrawing: () => void;
}

export function usePlayback(
    frames: ReplayFrame[],
    reverses: (AnimationSequenceMessage | null)[],
    times: (number | null)[],
    pen: PenKeys,
): Playback {
    const { updateGameState } = useServerActions();
    const { isAnimating, resetAnimations } = useAnimation();

    const [stepIdx, setStepIdx] = useState(-1); // -1 = pre-deal
    const [playing, setPlaying] = useState(false);
    const speeds = useMemo(() => buildSpeeds(times), [times]);
    const [speedIdx, setSpeedIdx] = useState(0);
    // wall-clock target for the next autoplay move (realtime waits can exceed
    // setTimeout's 2^31 ms ceiling, so we tick against Date.now() instead)
    const [waitTarget, setWaitTarget] = useState<number | null>(null);
    const [now, setNow] = useState(() => Date.now());
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;
    const lastIdx = frames.length - 1;

    // publish one step's sequence into the feed; a fresh sequence_id (and a
    // deep copy) lets the same step replay after scrubbing back. Plain
    // counter + Math.random - crypto.randomUUID needs a secure context and
    // breaks LAN dev on iOS (http://192.168.x.x).
    const publishSeq = useRef(0);
    const publishStep = useCallback(
        (i: number) => {
            const seq: AnimationSequenceMessage = structuredClone(frames[i].seq);
            seq.sequence_id = `replay-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
            seq.timestamp = Date.now();
            (seq.events[0] as any)._nonce = seq.sequence_id; // defeat content dedup
            animationFeed.publish(seq);
            setStepIdx(i);
        },
        [frames],
    );

    const stepForward = useCallback(() => {
        setWaitTarget(null);
        if (stepRef.current >= lastIdx) {
            setPlaying(false);
            return;
        }
        publishStep(stepRef.current + 1);
    }, [lastIdx, publishStep]);

    // one step back plays the reverse sequence (cards fly home) and lands on
    // steps[i-1]; mirrors publishStep but with the inverted flight
    const publishReverse = useCallback(
        (i: number) => {
            const rev = reverses[i];
            if (!rev) return;
            const seq: AnimationSequenceMessage = structuredClone(rev);
            seq.sequence_id = `replay-rev-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
            seq.timestamp = Date.now();
            if (seq.events[0]) (seq.events[0] as any)._nonce = seq.sequence_id;
            animationFeed.publish(seq);
            setStepIdx(i - 1);
        },
        [reverses],
    );

    // seeking commits the target state directly: drop in-flight animations so
    // a stale event can't overwrite the jumped-to state afterwards
    const jumpTo = useCallback(
        (i: number) => {
            setPlaying(false);
            setWaitTarget(null);
            resetAnimations();
            const target = Math.max(0, Math.min(i, lastIdx));
            // The step's own board, straight from the kernel - no rebuild.
            updateGameState(REPLAY_KEY, frames[target].game);
            setStepIdx(target);
        },
        [frames, lastIdx, resetAnimations, updateGameState],
    );

    const stepBack = useCallback(() => {
        setPlaying(false);
        setWaitTarget(null);
        if (stepRef.current <= 0) {
            jumpTo(0);
            return;
        }
        publishReverse(stepRef.current);
    }, [publishReverse, jumpTo]);

    // Bout boundaries: a bout begins at the ATTACK that opens onto an empty
    // table (the previous step cleared it via pickup/discard, or it's the
    // game's first attack). Skip-to-bout jumps are animationless seeks.
    const boutStarts = useMemo(() => {
        const starts: number[] = [];
        for (let i = 0; i < frames.length; i++) {
            const opensEmpty = i === 0 || frames[i - 1].game.battles.length === 0;
            if (frames[i].kind === REPLAY_STEP.ATTACK && opensEmpty) starts.push(i);
        }
        return starts;
    }, [frames]);

    const nextBout = useCallback(() => {
        const from = Math.max(0, stepRef.current);
        const next = boutStarts.find((s) => s > from);
        jumpTo(next ?? lastIdx);
    }, [boutStarts, lastIdx, jumpTo]);

    // start of the current bout; if already sitting on it, fall back to the
    // previous bout's start (the usual transport-deck behaviour)
    const boutStart = useCallback(() => {
        const from = Math.max(0, stepRef.current);
        const here = [...boutStarts].reverse().find((s) => s <= from) ?? 0;
        if (here < from) {
            jumpTo(here);
            return;
        }
        const prev = [...boutStarts].reverse().find((s) => s < from);
        jumpTo(prev ?? 0);
    }, [boutStarts, jumpTo]);

    // opening deal on mount
    useEffect(() => {
        const timer = setTimeout(() => publishStep(0), 400);
        return () => clearTimeout(timer);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // autoplay scheduling: once the previous event's animation lands, pick the
    // delay before the next move - condensed beats by default, or the recorded
    // gap divided by the dial speed in realtime modes - and arm a wall-clock
    // target. The ticker below fires it; this survives day-long waits.
    useEffect(() => {
        if (!playing || isAnimating || waitTarget !== null) return;
        if (stepIdx >= lastIdx) {
            setPlaying(false);
            return;
        }
        const mult = speeds[speedIdx % speeds.length].mult;
        let delay = 250;
        const a = stepIdx >= 0 ? times[stepIdx] : null;
        const b = times[stepIdx + 1];
        if (a !== null && b !== null && b !== undefined) {
            const gapMs = Math.max(0, (b - a) * 1000);
            delay =
                mult === null
                    ? Math.min(Math.max(gapMs, 150), 3000)
                    : Math.max(gapMs / mult, 30);
        }
        setWaitTarget(Date.now() + delay);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [playing, isAnimating, stepIdx, lastIdx, speedIdx, speeds, times, waitTarget]);

    // the ticker: fires the armed move and drives the countdown display
    useEffect(() => {
        if (waitTarget === null) return;
        const tick = () => {
            const t = Date.now();
            setNow(t);
            if (t >= waitTarget) {
                stepForward();
            }
        };
        tick();
        const interval = setInterval(tick, 250);
        return () => clearInterval(interval);
    }, [waitTarget, stepForward]);

    const { drawing, toggleDrawing } = pen;
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            // 'c' (comment) cycles the telestrator: enter draw mode, then exit
            // + clear, then a fresh blank overlay again.
            if (e.key === 'c' || e.key === 'C') {
                toggleDrawing();
                return;
            }
            // While drawing, the transport keys are inert so a commentator's
            // keystrokes can't scrub or play the replay underneath the pen.
            if (drawing) return;
            if (e.key === 'ArrowRight') stepForward();
            if (e.key === 'ArrowLeft') stepBack();
            if (e.key === ' ') {
                e.preventDefault();
                setPlaying((p) => !p);
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [stepForward, stepBack, drawing, toggleDrawing]);

    const nextSpeed = useCallback(() => {
        setWaitTarget(null); // re-arm with the new speed
        setSpeedIdx((i) => (i + 1) % speeds.length);
    }, [speeds.length]);

    return {
        stepIdx, lastIdx, frame: frames[Math.max(0, stepIdx)],
        playing, setPlaying, waitTarget, now,
        speeds, speedIdx, nextSpeed,
        stepForward, stepBack, jumpTo, nextBout, boutStart,
    };
}
