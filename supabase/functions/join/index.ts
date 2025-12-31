import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { ANIMATION_EVENT_TYPE } from "../_shared/types.ts";

wrap400(async ({user, user_name, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { game_id } = body;

    if (game.status !== 'waiting') {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    // Check if player is already in game
    if (game.players.some(p => p.player_id === user_id)) {
        throw new Error(`Player ${user_id} is already in game ${game_id}`);
    }

    // Add player to game
    game.players.push({
        player_id: user_id,
        name: user_name,
        status: 'idle',
        is_ai: false,
        hand: [],
        awaiting_attack: false,
        done_attacking_this_round: false,
        hand_length: 0,
        strategy_key: 'human'
    });

    // Add animation event to notify other players
    // Note: saveCompleteGame (called by executeWithGameLock) will handle:
    // - Updating games table with new players array
    // - Upserting player_hands for the new player
    // No additional DB operations needed here!

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${user_name} joined the game`,
        game_state: game
    }] };

}, false);

