/* =============================================================================
 * tutorialBeats.ts - the tutorial's narration, decided from the frames
 * =============================================================================
 * Its own module and not part of Tutorial.tsx, because it is a pure function of
 * the replay and the component is not: importing the component to test this
 * pulls in the providers and, through them, a Supabase client that wants a URL.
 * That is why it went untested, and untested is how the trump-cover beat came
 * to read a pair the way e2e/tutorial_game.test.ts warns against.
 * ========================================================================== */

import type { ReplaySummary } from '@sdk/ts/wasm/bots.ts';
import { ReplayFrame, REPLAY_STEP } from '../replay/frames.ts';
import type { StringId } from '../localization/strings';

/** The learner always sits at seat 0 in the frozen tutorial game. */
const LEARNER_SEAT = 0;

/* ----------------------------- concept beats ------------------------------- */

/* The tutorial's own narration, in the one string table (c/i18n/keys.h, the
 * `tut_` keys). It used to be a second table in this directory with three
 * languages of its own; a learner reading one of the other twenty-two was
 * taught in English while the board around them spoke their language. The
 * narration is also why these keys are NOT the board's: `cover` is a button a
 * player presses, `tut_cover` is a sentence explaining what covering is, and
 * the two are different words in most languages. */
export type TutKey = Extract<StringId, `tut_${string}`>;

export interface Beat { at: number; key: TutKey; extras?: TutKey[]; name?: string; }

/* ONE STEP SHOWS ONE BEAT, so two beats on one step is one lesson lost.
 * Tutorial.tsx takes the LAST beat at or before the cursor, so of two pushed
 * against the same step the second silently wins. The shipped tutorial had two
 * such collisions, and the first of them cost the learner the definition of
 * covering: their opening cover is also their trump cover, so `tut_cover` and
 * `tut_stack_rule` were built, overwritten by `tut_trump_cover`, and never
 * read. The other dropped `tut_out` under `tut_deck_empty`.
 *
 * The builder already had the answer for the one collision it knew about - a
 * round end is both "good" and "discard", so it pushed ONE beat carrying both -
 * and this applies that rule to every collision instead of to the one that was
 * noticed. The first beat on a step leads, because it is the one the step is
 * really about (you covered) and the riders qualify it (with a trump); its
 * `name` is the one that survives, and the riders are keys without names. */
function collapse(beats: Beat[]): Beat[] {
    const out: Beat[] = [];
    for (const b of beats) {
        const head = out[out.length - 1];
        if (head && head.at === b.at) {
            head.extras = [...(head.extras ?? []), b.key, ...(b.extras ?? [])];
        } else {
            out.push({ ...b });
        }
    }
    return out;
}

/* A concept is taught the first time the game shows it.
 *
 * A step is one ACTION now, and an action brings its consequences with it, so
 * the concepts that used to be steps of their own are read off the step's own
 * EVENTS instead: a refill is what a draw looks like, an out is what going out
 * looks like. Both are the kernel's own events — the beat asks the frame what
 * happened, it does not re-derive it.
 *
 * At most one beat shows at a time (the latest at or before the cursor), so a
 * step that teaches two things at once — a round end is both "good" and
 * "discard" — would silently drop one. Rather than lose it, that step gets ONE
 * beat carrying both. */
export function buildBeats(frames: ReplayFrame[], summary: ReplaySummary, names: string[]): Beat[] {
    const beats: Beat[] = [];
    const seen = new Set<string>();
    const once = (k: string) => (seen.has(k) ? false : (seen.add(k), true));
    const ps = summary.powerSuit;
    const fa = summary.firstAttacker;
    beats.push({ at: 0, key: fa === LEARNER_SEAT ? 'tut_first_attacker_you' : 'tut_first_attacker', name: names[fa] });

    const has = (f: ReplayFrame, type: string) => f.seq.events.some((e) => e.type === type);

    for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        const prev = frames[i - 1];
        switch (f.kind) {
            case REPLAY_STEP.ATTACK:
                if (prev && prev.game.battles.length > 0 && once('throwIn'))
                    beats.push({ at: i, key: 'tut_throw_in', extras: ['tut_capacity'] });
                break;
            case REPLAY_STEP.COVER: {
                if (once('cover')) beats.push({ at: i, key: 'tut_cover', extras: ['tut_stack_rule'] });
                // EVERY pair of the move: one step can take several attacks
                // (buildReplayFrames merges a multi-cover the wire split), and
                // the trump may be spent on any of them - reading cards[0]
                // against a single target would miss it and, worse, compare one
                // pair's card with another pair's attack.
                if ((f.pairs ?? []).some((pr) => pr.card.suit === ps && pr.target.suit !== ps)
                    && once('trumpCover'))
                    beats.push({ at: i, key: 'tut_trump_cover' });
                break;
            }
            case REPLAY_STEP.PASS: if (once('pass')) beats.push({ at: i, key: 'tut_pass' }); break;
            case REPLAY_STEP.PICKUP: if (once('pickup')) beats.push({ at: i, key: 'tut_pickup' }); break;
            case REPLAY_STEP.GOOD: if (once('good')) beats.push({ at: i, key: 'tut_good' }); break;
            case REPLAY_STEP.ROUND_END: {
                // The bout closed: everyone said good, and the table was binned.
                const g = once('good'), d = once('discard');
                if (g) beats.push({ at: i, key: 'tut_good', extras: d ? ['tut_discard'] : undefined });
                else if (d) beats.push({ at: i, key: 'tut_discard' });
                break;
            }
        }
        // Draws and outs ride the action that caused them.
        if (has(f, 'refill') && once('draw')) beats.push({ at: i, key: 'tut_draw' });
        if (has(f, 'out') && once('out')) {
            const outEv = f.seq.events.find((e) => e.type === 'out');
            const seat = outEv?.seat ?? -1;
            beats.push({ at: i, key: 'tut_out', name: names[seat >= 0 ? seat : 0] });
        }
        if (f.game.deckCount === 0 && !f.game.hasFlipped && once('deckEmpty'))
            beats.push({ at: i, key: 'tut_deck_empty' });
        if (i === frames.length - 1) beats.push({ at: i, key: 'tut_fool', name: names[summary.fool] });
    }
    beats.sort((a, b) => a.at - b.at);
    return collapse(beats);
}
