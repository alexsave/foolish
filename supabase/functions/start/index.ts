import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { start_game, cloneGame } from "../_shared/common_utils.ts";
import { ANIMATION_EVENT_TYPE, PLAYER_STATUS, GAME_STATUS } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";
import { AnimationEvent } from "../_shared/types.ts";

wrap400(async ({ user, game }: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle player ready logic
    if (game.status !== GAME_STATUS.WAITING) {
        return { game, events: [] };
    }

    // Set this player's status to ready
    const player = game.players.find(p => p.player_id === user_id);
    if (player) {
        player.status = PLAYER_STATUS.READY;
    }

    // Check if ALL players are ready AND we have at least 2 players
    const allPlayersReady = game.players.every(p => p.status === PLAYER_STATUS.READY) && game.players.length >= 2;

    if (allPlayersReady) {
        // start_game emits its own leading MAGIC_TRANSITION (status=PLAYING,
        // empty hands) so DEAL animations land in the right view.
        return { game, events: start_game(game) };
    }

    // Not yet starting — just notify others that this player is ready.
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${player?.name} is ready`,
        game_state: cloneGame(game)
    }] };

}, false);
