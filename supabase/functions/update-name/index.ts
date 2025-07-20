import { wrap400 } from "../_shared/utils.ts";
import { Game, GAME_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { new_name } = body;

    // Load complete game state from separated tables
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Can only update name during waiting phase
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only update name during game lobby`);
    }

    // Validate new name
    if (!new_name || typeof new_name !== 'string' || new_name.trim().length === 0) {
        throw new Error('New name must be a non-empty string');
    }

    if (new_name.length > 50) {
        throw new Error('Name must be 50 characters or less');
    }

    // Update game name
    game.name = new_name.trim();

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);

    return { game, events: [] };

}, false); 