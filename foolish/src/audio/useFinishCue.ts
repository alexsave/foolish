// useFinishCue.ts - the end of a game, heard once, as the results come up.
//
// The plan cannot sound this: a game ending is not a step, it is the board's
// status changing, and the screen that swaps the board for the results is what
// knows the moment (src/components/GameView.tsx). WHICH sound it is - the
// fool's, or everybody else's - is the kernel's (anim_finish_cue).

import { useEffect, useRef } from 'react';
import { animFinishCue } from '@sdk/ts/wasm/bots.ts';
import { ANIM_CUE_NONE } from '@sdk/ts/gen/anim.bots.ts';
import { GAME_STATUS, type TableView } from '../state/view';
import { cueAudio } from './cueAudio';

export function useFinishCue(view: TableView | null | undefined): void {
    // The game this screen last showed, and whether it was over then.
    const seen = useRef<{ gameId: string; over: boolean } | null>(null);
    const gameId = view?.gameId, status = view?.status;
    useEffect(() => {
        if (!view || gameId === undefined) return;
        const over = status === GAME_STATUS.GAME_OVER;
        const was = seen.current;
        seen.current = { gameId, over };
        // ONLY A GAME WATCHED ENDING. Opening a game that finished yesterday
        // shows the same results screen and is not its end; neither is a
        // different game's board replacing this one.
        if (!was || was.gameId !== gameId || was.over || !over) return;
        const cue = animFinishCue(view.fool, view.mySeat);
        if (cue !== ANIM_CUE_NONE) cueAudio.strike(cue);
        // `view` is read for the two seats only, at the moment the status turns.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [gameId, status]);
}
