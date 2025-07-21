import { wrap400, ExecutionParams, animationEvents } from "../_shared/utils.ts";
import { GAME_STATUS } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { new_order } = body;

    console.log('Rearrange players debug:', {
        body: JSON.stringify(body),
        new_order: JSON.stringify(new_order),
        new_order_type: typeof new_order,
        new_order_length: new_order?.length,
        game_players_length: game.players.length,
        game_players: game.players.map(p => p.player_id)
    });

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Can only rearrange during waiting phase
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only rearrange players during game lobby`);
    }

    // Validate new order - must contain all current players
    if (!Array.isArray(new_order) || new_order.length !== game.players.length) {
        console.error('Validation failed:', {
            is_array: Array.isArray(new_order),
            new_order_length: new_order?.length,
            game_players_length: game.players.length
        });
        throw new Error(`New order must contain exactly ${game.players.length} player IDs`);
    }

    // Verify all player IDs are valid
    for (const player_id of new_order) {
        if (!game.players.some(p => p.player_id === player_id)) {
            throw new Error(`Player ID ${player_id} not found in game`);
        }
    }

    // Rearrange players according to new order
    const rearrangedPlayers = new_order.map(player_id => 
        game.players.find(p => p.player_id === player_id)!
    );

    game.players = rearrangedPlayers;

    // Add animation event to notify all players about the rearrangement
    const userPlayer = game.players.find(p => p.player_id === user_id);
    const userName = userPlayer?.name || 'Someone';
    animationEvents.addMagicTransitionEvent(`${userName} rearranged the player order`, game);

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, false); 