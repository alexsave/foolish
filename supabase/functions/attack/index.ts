import { wrap400, verify_game_id, loadCompleteGame, saveCompleteGame, broadcastToGameUsers, verify_player_in_game, personalize_game, cardDisplay, validate_defender_status, verify_cards_in_players_hand, no_cards_left, check_win } from "../_shared/utils.ts";
import { Game, Card, Player, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS } from "../_shared/types.ts";
import { emailToName } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

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

    // Load complete game state from separated tables
    let game = await loadCompleteGame(game_id);

    // Handle attack logic
    game = handle_attack(game, game_id, user_id, cards);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
        message: `Player ${user_name} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
        player_id: user_id
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




// previous attack


const handle_attack = (game: Game, game_id: string, player_id: string, cards: Card[]): Game => {
    //const public_game_channel = getPublicGameChannel();
    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check if cards all have same value. this is kinda iffy because you could put down multiple cards
    // at the same time as long as the values are on the board
    // But this also slows down attackign to make it more fair for all attackers
    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    const player: Player = game.players.find(player => player.id === player_id)!;

    // also the attacker cannot be the defender
    validate_defender_status(game, player_id, false);

    // check if every card is in hand
    verify_cards_in_players_hand(player, cards);

    // make sure there are enough cards in the defenders hand
    let uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;

    const defender: Player = game.players[game.currently_attacked];

    let defender_cards = defender.hand.length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error(`Player ${player_id} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    let broadcast_message: any | null = null;

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // check if player is first attacker
        if (game.players[game.first_attacker].id !== player_id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }

        // Ok passed checks, we can put the cards on the table
        // remove from hand, put on table
        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));

        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }

        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            check_win(game);

            broadcast_message = {
                type: 'player_wins',
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}, getting rid of all their cards`,
            }

        } else {
            broadcast_message = {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
            }
        }

        // Ok now that it's on the table, we set the status to "free for all"
        // Defender can pick up, cover, pass
        // All attackers can attack
        // Whatever comes in first comes first, otherwise gg
        game.status = GAME_STATUS.FREE_PLAY;


        // check win later, becuase a "first attack" could win, putting the game into idle
    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // This is very similar to the above, we just don't check if they are the first attacker
        // attack + free_play means you can do whatever

        // every value has to be on the table
        if (!cards.every(card => game.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            throw new Error(`Some card values of ${cards.map(card => cardDisplay(card)).join(', ')} are not on the table`);
        }
        // a valid attack will move us out of wait_for_attackers
        game.players.forEach(player => {
            if (player.status === PLAYER_STATUS.AWAITING_ATTACK) {
                player.status = PLAYER_STATUS.IN;
            }
        });
        game.status = GAME_STATUS.FREE_PLAY;

        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));
        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }


        // It's possible they win here
        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            check_win(game);

            broadcast_message = {
                type: 'player_wins',
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}, getting rid of all their cards`,
            }
        } else {
            broadcast_message = {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
            }

        }

        uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
        defender_cards = defender.hand.length;

        // it's important to check if we need to shift to only_defend
        if (uncovered_cards === defender_cards) {
            // just reached the limit
            game.status = GAME_STATUS.ONLY_DEFEND;
            broadcastToGameUsers(game, 'game_update', {
                type: 'no_more_attacks',
                message: `Maximum number of attacks reached, only defender can defend`,
            });
        } else if (uncovered_cards > defender_cards) {
            // how the fuck did this happen
            throw new Error('SEVERE: Uncovered cards > defender_cards');
        }


        //} else if (game.status === GAME_STATUS.ONLY_DEFEND) {
        // just reject
    } else {
        throw new Error(`Player ${player_id} tried to attack but game is not in valid state`);
        // handle others later
    }

    // Not the best way but decent
    if (broadcast_message !== null) {
        broadcastToGameUsers(game, 'game_update', {
            ...broadcast_message, 
        });
    }

    return game;
}
