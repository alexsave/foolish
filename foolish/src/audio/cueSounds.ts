// cueSounds.ts - WHICH FILES EACH SOUND CUE PLAYS, AND HOW LOUD. The one place
// a sound is swapped.
//
// The kernel decides which cue a beat makes and when (c/src/anim_plan.h "sound
// cues"); this table decides only what that cue SOUNDS LIKE on the web. Nothing
// else in src/ names an audio file, so replacing a sound with a recording is an
// edit to one row here and to nothing else - docs/AUDIO.md walks through it.
//
// THE FILES. A path is relative to /audio/ (public/audio/ in the repo).
//   gen/...        SYNTHESISED by tools/soundgen at build time and never
//                  committed: the card models Alex approved by ear, and the
//                  stand-ins for the cues with no model yet. soundgen also
//                  writes their list (./gen/cueTakes.ts), which this reads.
//   anything else  a RECORDING: a committed source file. WAV, or whatever else
//                  every target browser's decodeAudioData reads (m4a/AAC and
//                  mp3 everywhere; ogg not in Safari).
//
// A TAKE IS A FILE AND ITS ANCHOR. A card thrown in is a grab and a thumb slide
// BEFORE the card leaves, so the take struck at that moment starts before it:
// `anchorMs` is how far into the file the cue's moment is, and the engine
// starts the file that much early. SEVERAL TAKES IN A ROW ARE VARIANTS, played
// in turn at random so that a cue heard six times in a second is not one sample
// six times. A take is always played WHOLE: where the time before a moment is
// shorter than a take's anchor it is played late, and a row may carry TIGHT
// takes (its gesture pressed closer to the moment) for exactly that case. A
// row may also carry PRELUDES, the strokes that gather the table before a
// sweep or a pickup, one set per stroke count, played before its own take and
// over whatever is sounding then.
//
// A CUE MAPPED TO `null` IS SILENT ON PURPOSE. That is how a cue is turned off
// here without touching the kernel's list, whose ids never change.
//
// ANIM_CUE_REVERT HAS NO ROW, AND MUST NOT GET ONE. A refused move sounds like
// that move played backwards: the kernel's cue names the cue it undoes
// (AnimCue.undoes) and the engine plays THAT row's take reversed. So a
// recording dropped in for PLACE is also the sound of a refused attack, with
// nothing to keep in step.

import {
    ANIM_CUE_DEAL, ANIM_CUE_FLIP, ANIM_CUE_LIFT, ANIM_CUE_PLACE, ANIM_CUE_COVER,
    ANIM_CUE_PICKUP, ANIM_CUE_SWEEP, ANIM_CUE_PILE, ANIM_CUE_OUT,
    ANIM_CUE_GOOD, ANIM_CUE_HAND_OFF, ANIM_CUE_FINISH, ANIM_CUE_FOOL,
    ANIM_CUE_DEAL_LAND, ANIM_CUE_TURN, ANIM_CUE_PASS, ANIM_CUE_INTO_HAND,
} from '@sdk/ts/gen/kernel_consts.bots.ts';
import { CUE_CARDS_PER_LEVEL, CUE_MAX_ANCHOR_MS, CUE_TAKES, type CueTake } from './gen/cueTakes';

export type { CueTake };

export interface CueSound {
    /** The files, each with its anchor and level. */
    takes: readonly CueTake[];
    /** How loud this cue is against the others, 0..1. THE MIX: Alex sets it by
     *  ear on /sounds. The modelled cues are written at their models' own
     *  levels, so 1 is the level the model sets. */
    gain: number;
    /** How far a play's pitch may wander either way, as a fraction of its
     *  speed (0.06 is about a semitone). 0 for a cue with a pitch to keep, and
     *  for every modelled cue: its takes already vary as the recordings did. */
    pitch: number;
    /** How far a play's level may wander either way, as a fraction. */
    level: number;
    /** A cue that stands for several cards is this much louder for each card
     *  past the first, up to +60%. 0 when the cue's takes already say how many
     *  cards it is (the strokes of a sweep). */
    perCard: number;
    /** For a row with preludes (strokes before its take): the cards one stroke
     *  gathers. */
    cardsPerLevel: number;
}

/** Where the files are served from. */
export const CUE_SOUND_ROOT = '/audio/';

/** How early the engine may have to start a file: its longest anchor. */
export const CUE_MAX_LEAD_MS = CUE_MAX_ANCHOR_MS;

const takes = (kind: number): readonly CueTake[] => CUE_TAKES[kind] ?? [];
// A modelled card sound: its takes carry the model's own level and variation.
const model = (kind: number, gain = 1): CueSound =>
    ({ takes: takes(kind), gain, pitch: 0, level: 0, perCard: 0, cardsPerLevel: CUE_CARDS_PER_LEVEL[kind] ?? 0 });
// A synthesised stand-in, varied by the engine.
const standIn = (kind: number, gain: number): CueSound =>
    ({ takes: takes(kind), gain, pitch: 0.06, level: 0.18, perCard: 0, cardsPerLevel: 0 });
const note = (kind: number, gain: number): CueSound =>
    ({ takes: takes(kind), gain, pitch: 0, level: 0, perCard: 0, cardsPerLevel: 0 });

export const CUE_SOUNDS: Readonly<Record<number, CueSound | null>> = {
    [ANIM_CUE_DEAL]:      model(ANIM_CUE_DEAL),
    [ANIM_CUE_DEAL_LAND]: model(ANIM_CUE_DEAL_LAND),
    [ANIM_CUE_TURN]:      model(ANIM_CUE_TURN),
    [ANIM_CUE_FLIP]:      model(ANIM_CUE_FLIP),
    [ANIM_CUE_LIFT]:      model(ANIM_CUE_LIFT),
    [ANIM_CUE_PLACE]:     model(ANIM_CUE_PLACE),
    [ANIM_CUE_PASS]:      model(ANIM_CUE_PASS),
    [ANIM_CUE_COVER]:     model(ANIM_CUE_COVER),
    [ANIM_CUE_PICKUP]:    model(ANIM_CUE_PICKUP),
    [ANIM_CUE_INTO_HAND]: model(ANIM_CUE_INTO_HAND),
    [ANIM_CUE_SWEEP]:     model(ANIM_CUE_SWEEP),
    [ANIM_CUE_PILE]:      model(ANIM_CUE_PILE),
    // The placeholders, set against the modelled card set (Alex 2026-11-07: "turn them down"; they sat 10-20 dB
    // over most card sounds). Targets by loudest 50 ms RMS (`make -C tools/soundgen stats` times the gain), with a
    // card landing at about -22 dBFS and a deal at about -40: the game over -26 (once a game, just under a landing),
    // a seat going out -30, a good -34 and the hand-off -38 (heard every bout, so under the good).
    [ANIM_CUE_HAND_OFF]:  standIn(ANIM_CUE_HAND_OFF, 0.047),   // -11.4 dBFS raw -> -38
    [ANIM_CUE_OUT]:       note(ANIM_CUE_OUT, 0.068),           // -6.6 -> -30
    [ANIM_CUE_GOOD]:      note(ANIM_CUE_GOOD, 0.044),          // -6.8 -> -34
    [ANIM_CUE_FINISH]:    note(ANIM_CUE_FINISH, 0.1),          // -5.9 -> -26
    [ANIM_CUE_FOOL]:      note(ANIM_CUE_FOOL, 0.095),          // -5.6 -> -26
};

/** How long before its moment a cue's earliest take starts: when the frame loop
 *  must hand it over (src/audio/cueSchedule.ts). */
export function cueLeadMs(kind: number): number {
    let lead = 0;
    for (const t of CUE_SOUNDS[kind]?.takes ?? []) lead = Math.max(lead, t.anchorMs);
    return lead;
}

/** How many strokes a cue of `nCards` gathers in: one per `cardsPerLevel`
 *  cards, 1 at least (disc_game.py: one stroke per ~2.5 cards, 1-5), as many
 *  as its preludes have. 0 for a row with no preludes, or no cards. */
export function wantedLevel(sound: CueSound, nCards: number): number {
    if (!sound.cardsPerLevel || nCards <= 0) return 0;
    let top = 0;
    for (const t of sound.takes) if (t.role === 'prelude') top = Math.max(top, t.level);
    return Math.min(top, Math.max(1, Math.round(nCards / sound.cardsPerLevel)));
}

/**
 * The takes a strike may play with `leadMs` to go before its moment: the
 * cue's own (main) takes whose anchor fits, else its TIGHT takes (the gesture
 * pressed closer to its moment, so that played late it is late by as little as
 * can be), else any of its own. A revert (`backwards`) plays its own takes.
 * Never empty for a row with takes. Whatever is chosen is played whole.
 */
export function takesFor(sound: CueSound, leadMs: number, backwards = false): readonly CueTake[] {
    const main = sound.takes.filter((t) => t.role === 'main');
    if (backwards) return main;
    const fit = main.filter((t) => t.anchorMs <= leadMs);
    if (fit.length > 0) return fit;
    const tight = sound.takes.filter((t) => t.role === 'tight');
    return tight.length > 0 ? tight : main;
}

/** The strokes a cue of `nCards` is gathered in before its own take (a sweep,
 *  a pickup): the prelude takes of its level, none for a row with none. */
export function preludesFor(sound: CueSound, nCards: number): readonly CueTake[] {
    const level = wantedLevel(sound, nCards);
    return level > 0 ? sound.takes.filter((t) => t.role === 'prelude' && t.level === level) : [];
}
