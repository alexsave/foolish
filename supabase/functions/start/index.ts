import { loadCompleteGame, saveCompleteGame, wrap400, broadcastToGameUsers, verify_player_in_game, start_game, personalize_game } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, ServerEventType } from "../_shared/types.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state from separated tables
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }

    // let the error throw

    let message: string = `Player ${user_name} is ready`;
    let type: ServerEventType = SERVER_EVENT_TYPE.PLAYER_READY;

    // Update player status to ready
    game.players.find(player => player.player_id === user_id)!.status = PLAYER_STATUS.READY;

    if (game.players.length >= 2 && game.players.every(player => player.status === PLAYER_STATUS.READY)) {
        // We can start the game 
        game = await start_game(game);

        message = `Player ${user_name} is ready, starting game ${game_id}`;
        type = SERVER_EVENT_TYPE.GAME_STARTED;
        await saveCompleteGame(game);

    } else {
        // Just update player status without starting
        // We don't need to save EVERYTHING, just the public game
        // TODO save less
        await saveCompleteGame(game);
    }

    broadcastToGameUsers(game, 'game_update', {
        type: type,
        message: message,
        player_id: user_id
    });

    return {
        game: personalize_game(game, user_id)
    };
});
