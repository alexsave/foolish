import { wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { name } = body;

    if (!name || name.trim() === '') {
        throw new Error('Game name cannot be empty');
    }

    // Server-side validation: Check unicode character count
    const trimmedName = name.trim();
    if (Array.from(trimmedName).length > 20) {
        throw new Error('Game name must be 20 characters or less');
    }

    // Load the complete game
    //const game: Game = await loadCompleteGame(game_id);

    // Check if the game is in waiting status (only allow name changes in lobby)
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error('Game name can only be changed in the lobby');
    }

    // Check if the user is in the game (only players can change the name)
    const playerExists = game.players.some(player => player.player_id === user_id);
    if (!playerExists) {
        throw new Error('Only players in the game can change the name');
    }

    // Update the game name
    game.name = trimmedName;

    // Save the updated game
    //await saveCompleteGame(game);

    // Send broadcast notification
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.GAME_NAME_UPDATED,
        message: `Game name updated to "${trimmedName}" by ${user_name}`
    });

    //return {
    //    game: personalize_game(game, user_id)
    //};
}); 