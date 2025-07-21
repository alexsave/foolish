import { wrap400, ExecutionParams, animationEvents } from "../_shared/utils.ts";
import { PlayerHand } from "../_shared/types.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async ({user, user_name, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { game_id } = body;

    if (game.status !== 'waiting') {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    // Check if player is already in game
    if (game.players.some(p => p.player_id === user_id)) {
        throw new Error(`Player ${user_id} is already in game ${game_id}`);
    }

    // Add player to game
    game.players.push({
        player_id: user_id,
        name: user_name,
        status: 'idle',
        is_ai: false,
        hand: [],
        awaiting_attack: false,
        done_attacking_this_round: false,
        hand_length: 0
    });

    // Update game in database
    await supabaseClient
        .from('games')
        .update({ 
            players: game.players.map(p => ({
                player_id: p.player_id,
                name: p.name,
                status: p.status,
                is_ai: p.is_ai,
                hand_length: p.hand_length
            }))
        })
        .eq('id', game_id);

    // Initialize empty hand for new player
    await supabaseClient.from('player_hands').insert({
        game_id: game_id,
        player_id: user_id,
        hand: [],
        awaiting_attack: false
    } as PlayerHand);

    // Add animation event to notify other players
    animationEvents.addMagicTransitionEvent(`${user_name} joined the game`, game);

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, false);

