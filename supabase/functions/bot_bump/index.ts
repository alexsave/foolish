import { wrap400, ExecutionParams } from '../_shared/utils.ts';

// The wrap400 wrapper will automatically trigger lockedBotLoop since we have a game_id
wrap400(async (params: ExecutionParams) => ({ game: params.game, events: [] }));
