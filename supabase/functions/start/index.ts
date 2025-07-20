import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE, PLAYER_STATUS } from "../_shared/types.ts";
import { start_game } from "../_shared/utils.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";
import { animationEvents } from "../_shared/utils.ts";

wrap400(async ({user, user_name, game}: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle player ready logic
    let message = '';
    let type = '';
    
    if (game.status === 'waiting') {
        // Set this player's status to ready
        const player = game.players.find(p => p.player_id === user_id);
        if (player) {
            player.status = PLAYER_STATUS.READY;
        }
        
        // Check if ALL players are ready
        const allPlayersReady = game.players.every(p => p.status === PLAYER_STATUS.READY) && game.players.length >= 2;
        
        if (allPlayersReady) {
            // All players are ready - start the game!
            animationEvents.addMagicTransitionEvent(`All players ready - starting game!`);
            
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
            
            message = `All players ready - starting game ${game.id}`;
            type = SERVER_EVENT_TYPE.GAME_STARTED;
        } else {
            // Not all players ready yet - just mark this player as ready
            message = `Player ${user_name} is ready (${game.players.filter(p => p.status === PLAYER_STATUS.READY).length}/${game.players.length} ready)`;
            type = SERVER_EVENT_TYPE.PLAYER_READY;
        }
    }

    if (game.status === 'playing') {
        // Game is already playing, no action needed
        message = `Game ${game.id} is already in progress`;
        type = SERVER_EVENT_TYPE.PLAYER_READY;
    }

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, true);
