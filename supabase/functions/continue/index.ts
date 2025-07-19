import { wrap400 } from "../_shared/utils.ts";
import { Game, GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;

    // Load complete game state from separated tables
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.GAME_OVER) {
        throw new Error(`Game ${game.id} is not over`);
    }

    // Reset game state for new round
    game.status = GAME_STATUS.WAITING;
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IDLE;
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

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);

    // Determine message based on who won
    const winner = game.players.find(p => p.status === PLAYER_STATUS.OUT);
    const fool = game.players.find(p => p.status === PLAYER_STATUS.IN);
    
    let message = `Game ${game.id} has been reset for another round`;
    if (winner) {
        message = `Player ${winner.name} won! Game reset for another round`;
    } else if (fool) {
        message = `Player ${fool.name} was the fool! Game reset for another round`;
    }

    return { game, events: [] };

}, false); 