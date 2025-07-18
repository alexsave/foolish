import { wrap400, broadcastToGameUsers, start_game } from "../_shared/utils.ts";
import { Game, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS, PrivatePlayer, Bot } from "../_shared/types.ts";
import { MAX_PLAYERS } from "../_shared/constants.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import { verify_player_in_game, personalize_game } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;

    // Load complete game state from separated tables

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

    // Filter out bots that are already in the game
    const availableBots = bots.filter(bot => 
        !game.players.some(player => player.player_id === bot.id)
    );

    if (availableBots.length === 0) {
        throw new Error('No available bots (all bots are already in the game)');
    }

    const selectedBot: Bot = availableBots[Math.floor(Math.random() * availableBots.length)];

    // Create a bot player
    const botPlayer: PrivatePlayer = {
        player_id: selectedBot.id,
        name: selectedBot.nickname,
        status: PLAYER_STATUS.READY, // Bots are always ready
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        is_ai: true,
        done_attacking_this_round: false
    };

    // Add bot to game
    game.players.push(botPlayer);

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);
    if (game.players.length >= 2 && game.players.every(player => player.status === PLAYER_STATUS.READY)) {
        // We can start the game 
        /*game = */await start_game(game);

        //message = `Player ${user_name} is ready, starting game ${game_id}`;
        //type = SERVER_EVENT_TYPE.GAME_STARTED;
        //await saveCompleteGame(game);

    } //else {

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
        message: `Bot ${selectedBot.nickname} joined the game`,
        player_id: selectedBot.id
    });

    //return game;

}); 