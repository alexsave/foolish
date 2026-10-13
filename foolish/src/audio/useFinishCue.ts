// useFinishCue.ts - the end of a game, heard once, as the results come up.
//
// The plan cannot sound this: a game ending is not a step, it is the board's
// status changing, and the screen that swaps the board for the results is what
// knows the moment (src/components/GameView.tsx). WHICH sound it is - the
// fool's, everybody else's, or none because the game is still running - is the
// kernel's (anim_finish_cue).

import { useEffect, useRef } from 'react';
import { animFinishCue } from '@sdk/ts/wasm/bots.ts';
import { ANIM_CUE_NONE } from '@sdk/ts/gen/anim.bots.ts';
import type { TableView } from '../state/view';
import { cueAudio } from './cueAudio';

/** The kernel's answer for a board: the fool's seat (negative while the game
 *  runs) and the viewer's (negative for a spectator). */
export const finishCueOf = (view: Pick<TableView, 'fool' | 'mySeat'>): number =>
    animFinishCue(view.fool, view.mySeat);

export function useFinishCue(view: TableView | null | undefined): void {
    // The game this screen was showing before its status last changed.
    const watched = useRef<string | null>(null);
    const gameId = view?.gameId, status = view?.status;
    useEffect(() => {
        if (!view || gameId === undefined) return;
        const was = watched.current;
        watched.current = gameId;
        // ONLY A GAME WATCHED CHANGING. Opening a game that finished yesterday
        // shows the same results screen and is not its end; neither is another
        // game's board replacing this one. Whether the change that WAS watched
        // is the end of the game is not asked here: a board whose game is still
        // running has no fool, and the kernel answers NONE for it.
        if (was !== gameId) return;
        const cue = finishCueOf(view);
        if (cue !== ANIM_CUE_NONE) cueAudio.strike(cue);
        // `view` is read for the two seats only, at the moment the status turns.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [gameId, status]);
}
