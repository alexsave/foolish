import {wrap400, broadcastToGameUsers, verify_game_id, verify_player_in_game, personalize_game, cardDisplay, validate_defender_status, verify_cards_in_players_hand, no_cards_left, check_win, get_next_player_index, card_comp, broadcastToGame, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { Game, Card, SERVER_EVENT_TYPE, PLAYER_STATUS, PrivatePlayer, PublicPlayer} from "../_shared/types.ts";
import { emailToName } from "../_shared/common_utils.ts";

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

    // Verify game exists and player is in game
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Handle pass logic
    game = handle_pass(game, game_id, user_id, cards);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PASS_PLAYED,
        message: `Player ${user_name} passed using ${cards.map(card => cardDisplay(card)).join(', ')}`
    });

    // Return personalized game state
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


const handle_pass = (game: Game, game_id: string, player_id: string, cards: Card[]): Game => {
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
    //const player = game.players.find(player => player.id === player_id)!;

    // also the attacker has to be the defender
    validate_defender_status(game, player_id, true);

    const defender_id = player_id;
    const defender: PrivatePlayer = game.player_hands.find(hand => hand.player_id === defender_id)!;
    // lol variable name
    const publicDefender: PublicPlayer = game.players.find(player => player.id === defender_id)!;

    verify_cards_in_players_hand(defender, mCards);

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
    const next_player_hand: PrivatePlayer = game.player_hands.find(hand => hand.player_id === next_player.id)!;
    if (next_player_hand.hand.length < mCards.length + game.table_battles.length) {
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
    defender.hand = defender.hand.filter(card => !mCards.some(mCard => card_comp(card, mCard)));


    // If the deck is empty, they can get out here
    if (no_cards_left(game) && defender.hand.length === 0) {
        // they win
        publicDefender.status = PLAYER_STATUS.OUT;
        check_win(game);
        game.currently_attacked = next_player_index;
        broadcastToGameUsers(game, 'game_update', {
            type: SERVER_EVENT_TYPE.PLAYER_WON,
            message: `Player ${player_id} passed ${mCards.map(card => cardDisplay(card)).join(', ')} and got rid of all their cards`,
        });
    } else {
        game.currently_attacked = next_player_index;

        broadcastToGameUsers(game, 'game_update', {
            type: SERVER_EVENT_TYPE.PASS_PLAYED,
            message: `Player ${player_id} used ${mCards.map(card => cardDisplay(card)).join(', ')} to pass to ${next_player.name}`,
        });
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const new_defender_id = game.players[game.currently_attacked].id;
    // Lots of find calls. Maybe an intermediate type would be better for this
    const new_defender: PrivatePlayer = game.player_hands.find(hand => hand.player_id === new_defender_id)!;
    const defender_cards = new_defender.hand.length;

    // it's important to check if we need to shift to only_defend
    if (uncovered_cards === defender_cards) {
        // just reached the limit
        game.status = 'only_defend';
        broadcastToGameUsers(game, 'game_update', {
            type: 'no_more_attacks',
            message: `Maximum number of attacks reached, only defender can defend`,
        });
    } else if (uncovered_cards > defender_cards) {
        // how the fuck did this happen
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        // a pass could shift from only_defend to free_play
        game.status = 'free_play';
        broadcastToGameUsers(game, 'game_update', {
            type: 'free_play_mode',
            message: `Passed cards, now free play mode`,
        });
    }

    return game;
}