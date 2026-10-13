// cueSounds.ts - WHICH FILE EACH SOUND CUE PLAYS. The one place a sound is
// swapped.
//
// The kernel decides which cue a beat makes and when (c/src/anim_plan.h "sound
// cues"); this table decides only what that cue SOUNDS LIKE on the web. Nothing
// else in src/ names an audio file, so replacing a placeholder with a recording
// is an edit to one row here and to nothing else - docs/AUDIO.md walks through
// it.
//
// THE FILES. A path is relative to /audio/ (public/audio/ in the repo).
//   gen/<cue>_<n>.wav   PLACEHOLDERS, synthesised by tools/soundgen at build
//                       time and never committed. The stem is the kernel's own
//                       name for the cue in lower case.
//   anything else       a RECORDING: a committed source file. WAV, or whatever
//                       else every target browser's decodeAudioData reads
//                       (m4a/AAC and mp3 everywhere; ogg not in Safari).
//
// SEVERAL FILES IN A ROW ARE VARIANTS: takes of the same gesture, played in
// turn at random so that a cue heard six times in a second is not one sample
// six times. On top of that the engine moves every play's pitch and level a
// little (`pitch`, `level` below), so one file is enough to start with.
//
// A CUE MAPPED TO `null` IS SILENT ON PURPOSE. That is how a cue is turned off
// here without touching the kernel's list, whose ids never change.
//
// ANIM_CUE_REVERT HAS NO ROW, AND MUST NOT GET ONE. A refused move sounds like
// that move played backwards: the kernel's cue names the cue it undoes
// (AnimCue.undoes) and the engine plays THAT row's file reversed. So a
// recording dropped in for PLACE is also the sound of a refused attack, with
// nothing to keep in step.

import {
    ANIM_CUE_DEAL, ANIM_CUE_FLIP, ANIM_CUE_LIFT, ANIM_CUE_PLACE, ANIM_CUE_COVER,
    ANIM_CUE_PICKUP, ANIM_CUE_SWEEP, ANIM_CUE_PILE, ANIM_CUE_OUT,
    ANIM_CUE_GOOD, ANIM_CUE_HAND_OFF, ANIM_CUE_FINISH, ANIM_CUE_FOOL,
} from '@sdk/ts/gen/kernel_consts.bots.ts';

export interface CueSound {
    /** Paths under /audio/, one per variant. */
    files: readonly string[];
    /** How loud this cue is against the others, 0..1. Every placeholder is
     *  written at one peak level, so this column IS the mix. */
    gain: number;
    /** How far a play's pitch may wander either way, as a fraction of its
     *  speed (0.06 is about a semitone). 0 for a cue with a pitch to keep. */
    pitch: number;
    /** How far a play's level may wander either way, as a fraction. */
    level: number;
    /** A cue that stands for several cards (a sweep, a pickup) is this much
     *  louder for each card past the first, up to +60%. 0 when the cue's
     *  strikes are already counted one a card. */
    perCard: number;
}

/** Where the files are served from. */
export const CUE_SOUND_ROOT = '/audio/';

const card = (files: string[], gain: number, perCard = 0): CueSound =>
    ({ files, gain, pitch: 0.06, level: 0.18, perCard });
const note = (files: string[], gain: number): CueSound =>
    ({ files, gain, pitch: 0, level: 0, perCard: 0 });

export const CUE_SOUNDS: Readonly<Record<number, CueSound | null>> = {
    [ANIM_CUE_DEAL]:     card(['gen/deal_0.wav', 'gen/deal_1.wav'], 0.42),
    [ANIM_CUE_FLIP]:     card(['gen/flip_0.wav'], 0.6),
    [ANIM_CUE_LIFT]:     card(['gen/lift_0.wav'], 0.2),
    [ANIM_CUE_PLACE]:    card(['gen/place_0.wav', 'gen/place_1.wav'], 0.7),
    [ANIM_CUE_COVER]:    card(['gen/cover_0.wav', 'gen/cover_1.wav'], 0.7),
    [ANIM_CUE_PICKUP]:   card(['gen/pickup_0.wav'], 0.5, 0.06),
    [ANIM_CUE_SWEEP]:    card(['gen/sweep_0.wav'], 0.42, 0.06),
    [ANIM_CUE_PILE]:     card(['gen/pile_0.wav'], 0.5, 0.06),
    [ANIM_CUE_HAND_OFF]: card(['gen/hand_off_0.wav'], 0.3),
    [ANIM_CUE_OUT]:      note(['gen/out_0.wav'], 0.32),
    [ANIM_CUE_GOOD]:     note(['gen/good_0.wav'], 0.26),
    [ANIM_CUE_FINISH]:   note(['gen/finish_0.wav'], 0.4),
    [ANIM_CUE_FOOL]:     note(['gen/fool_0.wav'], 0.4),
};
