import { wrap400, emailToName, verify_game_id, verify_player_in_game, broadcastToGame, personalize_game, cardDisplay, validate_defender_status, verify_hands_in_players_hand, check_win, card_comp, canCover, refill, get_next_player_index, broadcastToUser } from "../_shared/utils.ts";
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
    const { game_id, cover_cards, attack_cards } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    let { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    game = handle_cover(game, game_id, user_id, cover_cards, attack_cards);

    // Attack can only update players and table_battles
    await supabaseClient.from('games').update({ players: game.players, table_battles: game.table_battles, status: game.status }).eq('id', game_id);


    broadcastToGame(game_id, {
        type: SERVER_EVENT_TYPE.COVER_PLAYED,
        message: `Player ${user_name} covered ${attack_cards.map(card => cardDisplay(card)).join(', ')} with ${cover_cards.map(card => cardDisplay(card)).join(', ')}`,
        game_id: game_id,
        game: personalize_game(game, null),
        player_id: user_id
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


const handle_cover = (game: Game, game_id: string, player_id: string, cover_cards: Card[], attack_cards: Card[]) => {
    // cover a card


    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    // ok now for the fun part
    // how should we handle this?
    // ok there's going to be 2 arrays of cards
    // the first one is the cards that are being covered
    // the second one is the cards that are being used to cover



    // ok first just make sure all the cards are in the hand
    verify_hands_in_players_hand(game.players[game.currently_attacked], cover_cards);

    // check no duplicates
    if (new Set(cover_cards).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }


    // ensure that each of the attack cards are on the table AND uncovered
    for (const card of attack_cards) {
        if (!game.table_battles.some(battle => battle.attack.value === card.value && battle.defense === null)) {
            throw new Error(`Card ${cardDisplay(card)} is not on the table`);
        }
    }

    // check no duplicates
    if (new Set(attack_cards).size !== attack_cards.length) {
        throw new Error(`Cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // ok now we know that the cards are in the hand and on the table
    // can they cover?
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        if (!canCover(attack_card, cover_card, game.power_suit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }

    // assert same size of arrays
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }

    let broadcast_message: any | null = null;

    // now cover the cards
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        // find the attack card on the table
        const attack_card_index = game.table_battles.findIndex(battle => card_comp(battle.attack, attack_card) && battle.defense === null);
        if (attack_card_index === -1) {
            // This shouldn't happen as we just validated
            throw new Error('SEVERE: Card not found on table');
        }
        game.table_battles[attack_card_index].defense = cover_card;
        broadcast_message = {
            type: SERVER_EVENT_TYPE.COVER_PLAYED,
            message: `Player ${player_id} covered ${cardDisplay(attack_card)} with ${cardDisplay(cover_card)}`,
        }
        // remove the cards from the hand
        //game.players[game.currentlyAttacked].hand = game.players[game.currentlyAttacked].hand.filter(card => !card_comp(card, cover_card));

    }

    // remove the cards from the hand
    game.players[game.currently_attacked].hand = game.players[game.currently_attacked].hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // There is one scenario where we instantly move on: the player has no cards left in their hand
    if (game.players[game.currently_attacked].hand.length === 0) {

        game.table_battles = []; // burn the cards. TODO keep track of HOW MANY cards are burned but not which
        refill(game);
        // and it's fucking tricky because they can win here
        // shift 
        game.first_attacker = game.currently_attacked;
        if (game.players[game.first_attacker].hand.length === 0) {
            // can't think right now, but we need better win checking 
            broadcast_message = {
                type: SERVER_EVENT_TYPE.PLAYER_WON,
                message: `Player ${game.players[game.first_attacker].name} got rid of their hand by covering with ${cover_cards.map(card => cardDisplay(card)).join(', ')}`,
            }
            // win if still empty after refill
            game.players[game.first_attacker].status = PLAYER_STATUS.OUT;
            check_win(game);
            game.first_attacker = get_next_player_index(game, game.first_attacker);
        }
        game.currently_attacked = get_next_player_index(game, game.first_attacker);
        return;
    }

    // only do this if all table cards are covered but the defender has cards left
    // we know they have cards left
    const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
    if (all_attacks_covered) {

        // so in the real game, we would give it like 15seconds to let other people throw down cards.
        // because this will be offline, we give them infinite time. 
        // to proceed the next round, do we need all players to agree? but it should be done in secret to avoid revealing values
        // Yeah I don't know how to make it not obvious that we're waiting for attackers because they have cards
        // Oh well
        game.status = GAME_STATUS.WAIT_FOR_ATTACKERS;

        // ok let's secretly see who can even play cards.
        // pretty simple. Because they just covered, the only cards that can be played are values on teh table
        const playable_values = new Set<number>();
        for (const battle of game.table_battles) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        // now we need to see who can play cards. not the defender lol
        const playable_players = game.players.filter(player => player.id !== game.players[game.currently_attacked].id && player.hand.some(card => playable_values.has(card.value)));
        if (playable_players.length === 0) {
            // no one can play cards
            // but don't make it that obvious. give it 30 seconds
            setTimeout(() => {
                //shift 

                broadcastToGame(game_id, {
                    type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
                    message: `Player ${player_id} successfully defended the attack`,
                    game: personalize_game(game, null)

                });
                
                game.table_battles = [];
                refill(game);
                game.first_attacker = game.currently_attacked;
                game.currently_attacked = get_next_player_index(game, game.first_attacker);
                game.status = GAME_STATUS.FIRST_ATTACKER;
            }, 5000 + Math.random() * 20000);
        } else {
            // someone can play cards
            // so we need to see who can play cards
            playable_players.forEach(player => {
                player.status = PLAYER_STATUS.AWAITING_ATTACK;
            });

            playable_players.forEach(player => {
                broadcastToUser(player.id, {
                    type: SERVER_EVENT_TYPE.PLAYABLE_CARDS,
                    message: `You can still play cards. Either play or confirm you are done attacking with "good"`,
                    game: personalize_game(game, player.id)
                });
            });

        }
    }
    if (broadcast_message) {
        broadcastToGame(game_id, { ...broadcast_message, game: personalize_game(game, null) });
    }

    return game;
}