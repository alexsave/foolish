import { wrap400 } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { start_game } from "../_shared/utils.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";
import { animationEvents } from "../_shared/utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;

    // Load complete game state using JOINs
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle player ready logic
    let message = '';
    let type = '';
    
    if (game.status === 'waiting') {
        // Add animation event for game starting
        animationEvents.addMagicTransitionEvent(`Game started by ${user_name}`);
        
        // Store player hands before starting for animation
        const playerHandsBefore = game.players.map(p => [...p.hand]);
        
        await start_game(game);
        
        // Add animation events for dealing cards to each player
        for (let i = 0; i < game.players.length; i++) {
            const player = game.players[i];
            const handBefore = playerHandsBefore[i];
            const newCards = player.hand.filter(card => 
                !handBefore.some(oldCard => oldCard.suit === card.suit && oldCard.value === card.value)
            );
            if (newCards.length > 0) {
                animationEvents.addDealEvent(player.player_id, newCards);
            }
        }
        
        // Add animation event for flipping the trump card
        if (game.flipped) {
            animationEvents.addFlippedEvent(game.flipped);
        }
        
        // Add animation event for defender position
        if (game.players[game.defender]) {
            animationEvents.addDefenderMoveEvent(game.players[game.defender].player_id);
        }
        
        message = `Player ${user_name} is ready, starting game ${game.id}`;
        type = SERVER_EVENT_TYPE.GAME_STARTED;
    }

    if (game.status === 'playing') {
        // Game is playing, player is ready
        message = `Player ${user_name} is ready for the game`;
        type = SERVER_EVENT_TYPE.PLAYER_READY;
    }

    if (game.status === 'playing') {

        message = `Player ${user_name} is ready, starting game ${game.id}`;
        type = SERVER_EVENT_TYPE.GAME_STARTED;
        //await saveCompleteGame(game);

    } else {
        // Just update player status without starting
        // We don't need to save EVERYTHING, just the public game
        // TODO save less
        //await saveCompleteGame(game);
    }

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, true);
