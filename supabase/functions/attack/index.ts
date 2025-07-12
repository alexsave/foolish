import { wrap400, loadCompleteGame, saveCompleteGame, broadcastToGameUsers } from "../_shared/utils.ts";
import { Game, Card, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { handleAttack } from "../_shared/actions/attack.ts";
import { verify_player_in_game, personalize_game, cardDisplay } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, cards } = body;

    // Load complete game state from separated tables
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle attack logic
    await handleAttack(game, user_id, cards);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
        message: `Player ${user_name} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
        player_id: user_id
    });

    return {
        game: personalize_game(game, user_id)
    };

});

