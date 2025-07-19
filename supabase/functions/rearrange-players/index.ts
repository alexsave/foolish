import { wrap400 } from "../_shared/utils.ts";
import { Game, GAME_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { new_order } = body;

    // Load complete game state from separated tables
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Can only rearrange during waiting phase
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only rearrange players during game lobby`);
    }

    // Validate new order - must contain all current players
    if (!Array.isArray(new_order) || new_order.length !== game.players.length) {
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

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);

    return { game, events: [] };

}, false); 