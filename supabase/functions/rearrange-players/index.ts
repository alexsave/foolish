import { loadCompleteGame, saveCompleteGame, personalize_game, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, SERVER_EVENT_TYPE, Game } from "../_shared/types.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, player_indices } = body;

    if (!Array.isArray(player_indices)) {
        throw new Error('player_indices must be an array');
    }

    // Load the complete game
    const game: Game = await loadCompleteGame(game_id);

    // Check if the game is in waiting status (only allow rearrangement in lobby)
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error('Players can only be rearranged in the lobby');
    }

    // Check if the user is in the game
    const playerExists = game.players.some(player => player.id === user_id);
    if (!playerExists) {
        throw new Error('Only players in the game can rearrange players');
    }

    // Validate indices array
    if (player_indices.length !== game.players.length) {
        throw new Error('Invalid player indices length');
    }

    // Check if all indices are valid and unique
    const sortedIndices = [...player_indices].sort((a, b) => a - b);
    const expectedIndices = Array.from({ length: game.players.length }, (_, i) => i);
    if (!sortedIndices.every((val, i) => val === expectedIndices[i])) {
        throw new Error('Invalid player indices');
    }

    // Rearrange players according to the indices
    const originalPlayers = [...game.players];
    game.players = player_indices.map((index: number) => originalPlayers[index]);

    // Save the updated game
    await saveCompleteGame(game);

    // Send broadcast notification
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYERS_REARRANGED,
        message: `Players rearranged by ${user_name}`
    });

    return {
        game: personalize_game(game, user_id)
    };
}); 