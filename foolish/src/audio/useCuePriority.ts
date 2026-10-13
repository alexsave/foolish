// useCuePriority.ts - tell the sound engine which cues could be heard next.
//
// Every sound is loaded, but the ones the next move could make are loaded
// first (src/audio/cueAudio.ts `want`). WHICH ones those are is a rule of the
// game, so it is the kernel's answer (c/src/anim_plan.h anim_cues_next): a
// lobby can only deal, an empty table can only be attacked, an uncovered card
// can be covered or taken, a bout that can close brings its sweep and refills.
//
// What is here is the asking. The rule reads seven scalars, all of which the
// board on screen already carries; this copies them out and passes the answer
// on whenever one of them changes.

import { useEffect } from 'react';
import { animCuesNext } from '@sdk/ts/wasm/bots.ts';
import { covered, type TableView } from '../state/view';
import { cueAudio } from './cueAudio';

/** The kernel's answer for a board: one bit per ANIM_CUE_*. */
export const cuesNextFor = (view: TableView): number => animCuesNext({
    status: view.status,
    mySeat: view.mySeat,
    nPlayers: view.seats.length,
    nBattles: view.battles.length,
    nCovered: view.battles.filter(covered).length,
    hasFlipped: view.hasFlipped,
    deck: view.deckCount,
});

export function useCuePriority(view: TableView | null | undefined): void {
    const status = view?.status, mySeat = view?.mySeat, nPlayers = view?.seats.length;
    const nBattles = view?.battles.length, nCovered = view?.battles.filter(covered).length;
    const hasFlipped = view?.hasFlipped, deck = view?.deckCount;
    useEffect(() => {
        if (view) cueAudio.want(cuesNextFor(view));
        // The seven scalars are the whole of what the rule reads.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [status, mySeat, nPlayers, nBattles, nCovered, hasFlipped, deck]);
}
