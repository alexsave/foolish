import { cueLeadUs, cueMix, cueTakes } from '@sdk/ts/wasm/bots.ts';
import { ANIM_SOUND_STAGGER_MS, ANIM_CUE_REVERT } from '@sdk/ts/gen/kernel_consts.bots.ts';
import { cueAudio, cueFile } from './cueAudio';

// Playing one cue on its own, for the workbenches: /sounds (SoundAudition) and the
// tuner panel on the replay screen (SoundTuner). Never used by the board.

/** Run `play` now, or as soon as every file of `kinds` has arrived: the first
 *  tap on this page is also the gesture that lets them be fetched. The board
 *  never waits like this - a sound that is late there is dropped - but a
 *  workbench is for hearing the sound, however long it took to arrive. */
export const whenLoaded = (kinds: readonly number[], play: () => void, tries = 40): void => {
    cueAudio.unlock();
    cueAudio.want(kinds.reduce((m, k) => m | (1 << k), 0));
    const ready = cueTakes().every((t) => !kinds.includes(t.cue) || cueAudio.loaded(cueFile(t.name)));
    if (ready || tries <= 0) return play();
    setTimeout(() => whenLoaded(kinds, play, tries - 1), 100);
};

/** The cue whose sound a cue is heard as: its own, or for a revert the one it undoes. */
export const voiced = (kind: number, undoes: number): number => (kind === ANIM_CUE_REVERT ? undoes : kind);

// One cue alone, its moment far enough off that its own takes fit (a cue with
// tight takes has a "Now" button too: its moment at once, as a move that opens
// its run strikes it, which plays a tight take, whole and late). A
// reversed take's lead is the part of it AFTER its anchor, which no table
// lists; a second covers every one.
export const strikes = (kind: number, n: number, undoes = 0, nCards = 1): void =>
    whenLoaded([voiced(kind, undoes)], () => {
        const lead = kind === ANIM_CUE_REVERT ? 1000 : cueLeadUs(voiced(kind, undoes)) / 1000 + 30;
        for (let h = 0; h < n; h++) cueAudio.strike(kind, lead + h * ANIM_SOUND_STAGGER_MS, nCards, undoes);
    });

/** A cue's strokes: the cards one stroke gathers, 0 for a cue with none. */
export const perStrokeOf = (kind: number): number => (cueMix(kind)?.cardsPerLevelPm ?? 0) / 1000;

/** One cue for the tuner, alone: whatever was sounding stops first (one sound at a time). */
export const tunerPlay = (kind: number, how: 'main' | 'tight' | number): void => {
    cueAudio.cancel();
    const per = perStrokeOf(kind);
    if (how === 'tight') whenLoaded([kind], () => cueAudio.strike(kind, 0));
    else if (typeof how === 'number') strikes(kind, 1, 0, Math.ceil(how * per));
    else strikes(kind, 1, 0, per > 0 ? 0 : 1);
};

