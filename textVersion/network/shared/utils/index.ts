// Utility functions for the game
import express from 'express';
import { 
    Game, Card, Player, PersonalGame, OtherPlayer, PlayerStatus, PLAYER_STATUS, 
    GAME_STATUS, Battle 
} from '../types';
import { 
    CARDS_PER_PLAYER, SUITS, START_VALUE, ACE_VALUE, VALUE_MAP, SUIT_MAP,
    LCG_A, LCG_C, LCG_M
} from '../constants';
import { getGames, getPublicGameChannel, getPrivateUserChannel } from '../database';

export const wrap400 = (execute: (req: express.Request, res: express.Response) => void) => (req: express.Request, res: express.Response) => {
    try {
        execute(req, res);
    } catch (e: any) {
        res.status(400).end(JSON.stringify({ error: e.message }));
    }
}

export const createId = () => {
    return crypto.randomUUID().slice(0, 6);
}


const other_player = (player: Player): OtherPlayer => {
    return { 
        name: player.name, 
        id: player.id, 
        hand_length: player.hand.length, 
        status: player.status === PLAYER_STATUS.AWAITING_ATTACK ? PLAYER_STATUS.IN : player.status 
    };
}

export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    return {
        deck_length: game.deck.length,
        flipped: game.flipped,
        self: game.players.find(player => player.id === player_id)!,
        players: game.players.map(other_player),
        status: game.status,
        firstAttacker: game.firstAttacker,
        currentlyAttacked: game.currentlyAttacked,
        previousFirstAttacker: game.previousFirstAttacker,
        previousCurrentlyAttacked: game.previousCurrentlyAttacked,
        table: game.table,
        powerSuit: game.powerSuit
    }
}






export const start_game = (game_id: string) => {
    const games = getGames();
    const public_game_channel = getPublicGameChannel();
    const private_user_channel = getPrivateUserChannel();

    // Assume that this is safe to call because we only call from server
    const game = games[game_id];

    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.deck = refill_deck();

    const hands = initialize_hands(game);
    for (let i = 0; i < game.players.length; i++) {
        game.players[i].hand = hands[i];

        private_user_channel.push({
            user_id: game.players[i].id,
            message: {
                type: 'player_hand',
                message: `Player ${game.players[i].name} hand ${game.players[i].hand.map(card => cardDisplay(card)).join(', ')}`,
                //hand: game.players[i].hand
            }
        });
    }

    let flipped_card = draw(game);
    while (flipped_card!.value === ACE_VALUE) {
        // move back to deck
        game.deck.push(flipped_card!);
        flipped_card = draw(game);
    }
    game.flipped = flipped_card;
    game.powerSuit = game.flipped!.suit;

    // Everyone needs to know
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'flipped_card',
            message: `Flipped card is ${cardDisplay(game.flipped!)}`,

        }
    });

    const lowest_power_index = determine_lowest_power_index(game);

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'first_attacker',
            message: `Player ${game.players[lowest_power_index].name} is the first attacker`
        }
    });

    game.firstAttacker = lowest_power_index;
    set_positions(game);


    // request attack from first attacker

    game.status = GAME_STATUS.FIRST_ATTACKER;
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'game_status',
            message: `Wait for first attacker to attack`
        }
    });
    private_user_channel.push({
        user_id: game.players[game.firstAttacker].id,
        message: {
            type: 'request_first_attack',
            message: `Please choose an attack. Options are ${game.players[game.firstAttacker].hand.map(card => cardDisplay(card)).join(', ')}`,
        }
    });

} 