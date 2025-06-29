import {wrap400, emailToName, broadcastToGame, verify_game_id, verify_player_in_game, personalize_game, cardDisplay, validate_defender_status, verify_hands_in_players_hand, no_cards_left, check_win, get_next_player_index, card_comp } from "../_shared/utils.ts";
import { Game, Card, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS } from "../_shared/types.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { createClient, User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

serve(wrap400(async (req) => {
    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    const user: User = await getAuthenticatedUser(req);
    const user_id = user.id;
    const user_name = emailToName(user.email);
    const { game_id, cards } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    let { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    game = handle_pass(game, game_id, user_id, cards);

    // Attack can only update players and table_battles
    await supabaseClient.from('games').update(game).eq('id', game_id);

    broadcastToGame(game_id, {
        type: SERVER_EVENT_TYPE.PASS_PLAYED,
        message: `Player ${user_name} passed using ${cards.map(card => cardDisplay(card)).join(', ')}`,
        game: personalize_game(game, null)
    }).catch(e => { 
        console.error('Error broadcasting game join:', e);
    });

    // Now it's safe to return - user has access to game channel
    return new Response(JSON.stringify({
        game: personalize_game(game, user_id)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

}));





// previous pass


const handle_pass = (game: Game, game_id: string, player_id: string, cards: Card[]) => {
    if (!cards) {
        throw new Error(`No cards provided`);
    }
    const mCards = cards;

    // check if cards all have same value. 
    if (!mCards.every(card => card.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(mCards).size !== mCards.length) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    const player = game.players.find(player => player.id === player_id)!;

    // also the attacker has to be the defender
    validate_defender_status(game, player_id, true);

    verify_hands_in_players_hand(player, mCards);

    // also important: THERE SHOULD BE CARDS ON THE TABLE
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability. 1. no cover, 2. all same value, 3. next player has enough cards
    // 1. no cover
    if (game.table_battles.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }
    // this also implies all same value on the table
    // and we already know that the pass cards are the same value
    // so check first pass card against all other cards on the tableo
    if (!game.table_battles.every(battle => battle.attack.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.currently_attacked);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < mCards.length + game.table_battles.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${mCards.map(card => cardDisplay(card)).join(', ')}`);
    }

    // Now we can pass
    // add to table
    //remove from hand
    // update currentlyAttacked

    for (const card of mCards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }
    player.hand = player.hand.filter(card => !mCards.some(mCard => card_comp(card, mCard)));


    // If the deck is empty, they can get out here
    if (no_cards_left(game) && player.hand.length === 0) {
        // they win
        player.status = PLAYER_STATUS.OUT;
        check_win(game);
        game.currently_attacked = next_player_index;
        broadcastToGame(game_id, {
            type: SERVER_EVENT_TYPE.PLAYER_WON,
            message: `Player ${player_id} passed ${mCards.map(card => cardDisplay(card)).join(', ')} and got rid of all their cards`,
            game: personalize_game(game, null)
        });
    } else {
        game.currently_attacked = next_player_index;

        broadcastToGame(game_id, {
            type: SERVER_EVENT_TYPE.PASS_PLAYED,
            message: `Player ${player_id} used ${mCards.map(card => cardDisplay(card)).join(', ')} to pass to ${next_player.name}`,
            game: personalize_game(game, null)
        });
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const defender_cards = game.players[game.currently_attacked].hand.length;

    // it's important to check if we need to shift to only_defend
    if (uncovered_cards === defender_cards) {
        // just reached the limit
        game.status = 'only_defend';
        broadcastToGame(game_id, {
            type: 'no_more_attacks',
            message: `Maximum number of attacks reached, only defender can defend`,
            game: personalize_game(game, null)
        });
    } else if (uncovered_cards > defender_cards) {
        // how the fuck did this happen
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        // a pass could shift from only_defend to free_play
        game.status = 'free_play';
        broadcastToGame(game_id, {
            type: 'free_play_mode',
            message: `Passed cards, now free play mode`,
            game: personalize_game(game, null)
        });
    }

    return game;
}