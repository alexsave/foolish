import { wrap400, loadCompleteGame, saveCompleteGame, broadcastToGameUsers, verify_player_in_game, personalize_game } from "../_shared/utils.ts";
import { Game, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS, PrivatePlayer, Bot } from "../_shared/types.ts";
import { MAX_PLAYERS } from "../_shared/constants.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state from separated tables
    let game: Game = await loadCompleteGame(game_id);

    // Verify player is in game (only players in game can add bots)
    verify_player_in_game(game, user_id);

    // Ensure game is in waiting state
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error('Can only add bots to games in waiting state');
    }

    // Check if game is full (assuming max 6 players)
    if (game.players.length >= MAX_PLAYERS) {
        throw new Error('Game is full');
    }

    // Get all bots from the database and choose one randomly in JS
    const { data: bots, error } = await supabaseClient
        .from('bots')
        .select('*');

    if (error || !bots || bots.length === 0) {
        throw new Error('No bots available');
    }

    const selectedBot: Bot = bots[Math.floor(Math.random() * bots.length)];

    // Check if this bot is already in the game
    const botAlreadyInGame = game.players.some(player => player.player_id === selectedBot.id);
    if (botAlreadyInGame) {
        throw new Error('This bot is already in the game');
    }

    // Create a bot player
    const botPlayer: PrivatePlayer = {
        player_id: selectedBot.id,
        name: selectedBot.nickname,
        status: PLAYER_STATUS.READY, // Bots are always ready
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        is_ai: true
    };

    // Add bot to game
    game.players.push(botPlayer);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
        message: `Bot ${selectedBot.nickname} joined the game`,
        player_id: selectedBot.id
    });

    return {
        game: personalize_game(game, user_id),
        bot_added: selectedBot.nickname
    };

}); 