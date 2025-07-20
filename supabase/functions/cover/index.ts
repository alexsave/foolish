import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { handleCover } from "../_shared/actions/cover.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { cover_cards, attack_cards } = body;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle cover logic and return animation events
    const events = await handleCover(game, user_id, cover_cards, attack_cards);

    return { game, events };
}, true);

