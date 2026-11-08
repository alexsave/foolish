// cueSchedule.ts - which of a plan's sound cues are due, and how far off.
//
// The kernel's plan lists what a sequence sounds like and when, on the same
// clock the flights are on (c/src/anim_plan.h AnimPlan.cues). The frame loop
// (src/state/useAnimationRun.ts) re-asks the plan every frame; this is the one
// function that turns "the plan, and the run's clock right now" into "strike
// these, this many milliseconds from now", which the audio engine then pins to
// ITS clock (src/audio/cueAudio.ts). Pure, so it is tested without a browser.
//
// TWO NUMBERS ARE THE HOST'S, and both are about delivery, not about the game:
//
//   LOOK AHEAD  a cue is handed over this long before it is due. A frame loop
//               runs when the browser lets it - 16ms on a good day, 50 behind a
//               layout, never while a GC runs - and a sound started "now" from
//               a late frame is late by that much, audibly. A sound SCHEDULED a
//               little ahead on the audio clock is on time whatever the frames
//               do, as long as one of them ran inside the window.
//   LATE        a strike this far behind the clock is dropped rather than
//               played. A tab that was hidden comes back to a run that has
//               moved on (the loop lands every step it skipped at once), and
//               everything it missed must not be played in one burst.
//
// EVERY CUE IS HANDED OVER ONCE, by its (step, kind) - the pair the kernel
// promises is unique in a plan and stable as a run in flight is appended to.
//
// A FILE THAT STARTS BEFORE ITS MOMENT is handed over that much earlier. A take
// carries an anchor (the kernel's take table, c/src/cue_voice.h): the grab and
// the thumb slide come before the card leaves, the strokes that gather a table
// before it is swept. `leadMs(kind)` is the longest anchor a cue's takes have
// (cue_lead_us), and the look-ahead
// runs from the moment the file has to START, so the longest gathering is
// still scheduled on time whenever the plan knew about it that early.

import { ANIM_SOUND_STAGGER_MS } from '@sdk/ts/gen/kernel_consts.bots.ts';

export const CUE_LOOK_AHEAD_MS = 120;
export const CUE_LATE_MS = 80;

/** A cue as the plan lists one (sdk/ts/wasm/bots.ts AnimCueSnap). */
export interface PlanCue { atMs: number; kind: number; step: number; hits: number; nCards: number; undoes: number }

/** One strike to hand the engine. */
export interface CueStrike {
    kind: number;
    /** From now, on the run's clock. Never negative. */
    delayMs: number;
    nCards: number;
    /** The cue a REVERT plays backwards; ANIM_CUE_NONE otherwise. */
    undoes: number;
    /** The step that makes it (AnimCue.step), whose seat a landing is heard at. */
    step: number;
}

/** The key a cue is remembered by once it has been handed over. */
export const cueKey = (c: PlanCue): number => c.step * 256 + c.kind;

/**
 * The strikes to hand over at `nowMs` (from the run's start), given the cues
 * already handed over in `done`, which this ADDS to. A cue whose file must
 * start (`leadMs` before its first strike) further off than the look-ahead is
 * left for a later frame.
 */
export function dueStrikes(cues: readonly PlanCue[], nowMs: number, done: Set<number>,
                           leadMs: (kind: number) => number = () => 0): readonly CueStrike[] {
    let out: CueStrike[] | null = null;
    for (const c of cues) {
        if (c.atMs - Math.max(0, leadMs(c.kind)) - nowMs > CUE_LOOK_AHEAD_MS) continue;
        const key = cueKey(c);
        if (done.has(key)) continue;
        done.add(key);
        for (let h = 0; h < c.hits; h++) {
            const at = c.atMs + h * ANIM_SOUND_STAGGER_MS;
            if (at < nowMs - CUE_LATE_MS) continue;
            (out ??= []).push({
                kind: c.kind, delayMs: Math.max(0, at - nowMs), nCards: c.nCards, undoes: c.undoes,
                step: c.step,
            });
        }
    }
    return out ?? NO_STRIKES;
}

const NO_STRIKES: readonly CueStrike[] = [];
