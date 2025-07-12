import { wrap400, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { handleGood } from "../_shared/actions/good.ts";
import { verify_player_in_game, personalize_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle good logic
    handleGood(game, user_id);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    return {
        game: personalize_game(game, user_id)
    };
});

