import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { ANIMATION_EVENT_TYPE, GAME_STATUS, PLAYER_STATUS } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, game}: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.GAME_OVER) {
        throw new Error(`Game ${game.id} is not over`);
    }

    // Reset game state for new round
    game.status = GAME_STATUS.WAITING;
    game.players.forEach(player => {
        // Set status appropriately: bots to READY, humans to IDLE
        if (player.is_ai) {
            player.status = PLAYER_STATUS.READY;
        } else {
            player.status = PLAYER_STATUS.IDLE;
        }
        player.hand = [];
        player.hand_length = 0;
        player.awaiting_attack = false;
        player.done_attacking_this_round = false;
    });
    
    // Clear game state
    game.deck = [];
    game.discard_pile_length = 0;
    game.flipped = null;
    game.power_suit = 0;
    game.first_attacker = 0;
    game.defender = 0;
    game.table_battles = [];
    game.elimination_order = [];

    // Determine message based on who won
    const winner = game.players.find(p => p.status === PLAYER_STATUS.OUT);
    const fool = game.players.find(p => p.status === PLAYER_STATUS.IN);
    
    let message = `Game ${game.id} has been reset for another round`;
    if (winner) {
        message = `Player ${winner.name} won! Game reset for another round`;
    } else if (fool) {
        message = `Player ${fool.name} was the fool! Game reset for another round`;
    }

    // Add animation event to notify all players about the game reset
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: message,
        game_state: game
    }] };

}, false); 