import { cueLeadUs, cueMix, cueTakes } from '@sdk/ts/wasm/bots.ts';
import { ANIM_SOUND_STAGGER_MS, ANIM_CUE_REVERT } from '@sdk/ts/gen/kernel_consts.bots.ts';
import { CUE_ROLE_MAIN, CUE_ROLE_PRELUDE, CUE_ROLE_TIGHT } from '@sdk/ts/gen/view_layout.bots.ts';
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

/** One PART of a cue alone (Alex: "that's kind of the whole point of splitting each
 *  action into individual sounds"): soundgen renders that take again with every
 *  other part silent, at the whole set's common gain (`soundgen solo`, served by the
 *  tuner's endpoint), and it plays at the cue's mix gain through the master volume,
 *  so it is as loud as it is in the game. Each press plays the set's next take. */
const TUNE_URL = 'http://127.0.0.1:3101';
const nextTake = new Map<string, number>();
export const playPart = (kind: number, cue: string, role: string, part: string, strokes = 3): void => {
    cueAudio.cancel();
    const roleId = role === 'tight' ? CUE_ROLE_TIGHT : role === 'prelude' ? CUE_ROLE_PRELUDE : CUE_ROLE_MAIN;
    const n = cueTakes().filter((t) => t.cue === kind && t.role === roleId && (roleId !== CUE_ROLE_PRELUDE || t.level === strokes)).length;
    const key = `${cue} ${role} ${part} ${strokes}`;
    const take = (nextTake.get(key) ?? 0) % Math.max(1, n);
    nextTake.set(key, take + 1);
    const gain = (cueMix(kind)?.gainPm ?? 1000) / 1000;
    fetch(`${TUNE_URL}/solo/${cue}/${role}/${part}/${take}/${strokes}`, { cache: 'no-store' })
        .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error(`solo ${r.status}`))))
        .then((bytes) => cueAudio.playNow(bytes, gain))
        .catch((e) => console.warn('tuner: the part could not be played alone', e));
};

/** One cue for the tuner, alone: whatever was sounding stops first (one sound at a time). */
export const tunerPlay = (kind: number, how: 'main' | 'tight' | number): void => {
    cueAudio.cancel();
    const per = perStrokeOf(kind);
    if (how === 'tight') whenLoaded([kind], () => cueAudio.strike(kind, 0));
    else if (typeof how === 'number') strikes(kind, 1, 0, Math.ceil(how * per));
    else strikes(kind, 1, 0, per > 0 ? 0 : 1);
};

