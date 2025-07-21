import { wrap400, ExecutionParams, animationEvents } from "../_shared/utils.ts";
import { GAME_STATUS } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { new_name } = body;

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

    const oldName = game.name;

    // Update game name
    game.name = new_name.trim();

    // Add animation event to notify all players about the name change
    const userPlayer = game.players.find(p => p.player_id === user_id);
    const userName = userPlayer?.name || 'Someone';
    animationEvents.addMagicTransitionEvent(`${userName} changed game name from "${oldName}" to "${game.name}"`, game);

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, false); 