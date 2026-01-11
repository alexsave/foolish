import { wrap400, ExecutionParams } from '../_shared/utils.ts';

// The wrap400 wrapper will automatically trigger lockedBotLoop since we have a game_id
wrap400(async (params: ExecutionParams) => {
    // Also trigger auto-discard loop check (fire-and-forget)
    // The state we get into will trigger the auto-discard loop automatically in utils.ts
    return { game: params.game, events: [] };
}, true);
