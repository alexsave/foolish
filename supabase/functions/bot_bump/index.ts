import { wrap400, ExecutionParams } from '../_shared/utils.ts';
import { lockedAutoDiscardLoop } from '../_shared/auto_discard_loop.ts';

// The wrap400 wrapper will automatically trigger lockedBotLoop since we have a game_id
wrap400(async (params: ExecutionParams) => {
    // Also trigger auto-discard loop check (fire-and-forget)
    lockedAutoDiscardLoop(params.game.id).catch(error => {
        console.error(`Error starting auto-discard loop for game ${params.game.id}:`, error);
    });
    
    return { game: params.game, events: [] };
});
