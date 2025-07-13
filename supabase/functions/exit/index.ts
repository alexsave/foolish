import { loadCompleteGame, saveCompleteGame, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, Game, SERVER_EVENT_TYPE, PrivatePlayer } from "../_shared/types.ts";
import { personalize_game, verify_player_in_game } from "../_shared/common_utils.ts";

import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, bot_id } = body;

    // Load complete game state
    const game: Game = await loadCompleteGame(game_id);
    verify_player_in_game(game, user_id);

    // Determine which player to remove
    let playerToRemove: PrivatePlayer | undefined;
    let isRemovingBot = false;

    if (bot_id) {
        // Remove specified bot - only players in the game can do this
        playerToRemove = game.players.find(p => p.player_id === bot_id && p.is_ai);
        if (!playerToRemove) {
            throw new Error(`Bot ${bot_id} not found in game`);
        }
        isRemovingBot = true;
    } else {
        // Remove self - must be a player in the game
        playerToRemove = game.players.find(p => p.player_id === user_id);
        if (!playerToRemove) {
            throw new Error(`You are not in this game`);
        }
    }

    // Can only exit during waiting phase
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only exit during game lobby`);
    }

    // Remove player from game
    game.players = game.players.filter(p => p.player_id !== playerToRemove.player_id);

    // Remove from appropriate database table
    if (playerToRemove.is_ai) {
        // Remove bot hand
        await supabaseClient
            .from('bot_hands')
            .delete()
            .eq('game_id', game_id)
            .eq('bot_id', playerToRemove.player_id);
    } else {
        // Remove player hand
        await supabaseClient
            .from('player_hands')
            .delete()
            .eq('game_id', game_id)
            .eq('player_id', playerToRemove.player_id);
    }

    // Save updated game state
    await saveCompleteGame(game);

    // Send broadcast notification
    const messageText = isRemovingBot 
        ? `Bot ${playerToRemove.name} was removed from the game`
        : `${playerToRemove.name} left the game`;

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYER_LEFT_GAME,
        message: messageText,
        player_id: playerToRemove.player_id
    });

    // Return game state for the requesting user
    // If they removed themselves, they won't have self data (spectator mode)
    // If they removed a bot, they'll still have their self data
    const responseGame = isRemovingBot
        ? personalize_game(game, user_id)  // User removed a bot, they're still in game
        : {
            ...game,
            self: null // User removed themselves, now spectating
        };

    return {
        game: responseGame,
        removed_player: playerToRemove.name,
        is_spectator: !isRemovingBot  // User is spectating if they removed themselves
    };
}); 