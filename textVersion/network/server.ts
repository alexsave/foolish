import * as http from 'http';
import { refill_deck, Card, initialize_hands, draw, cardDisplay, determine_lowest_power_index, set_positions, CARDS_PER_PLAYER, canCover, ACE_VALUE } from './index';
import WebSocket from 'ws';
import { wrap400, createId, verify_game_id, verify_player_in_game, start_game, lobbify_game, User, PLAYER_STATUS, Game, Player, PlayerStatus, GAME_STATUS, GameStatus, GAME_MOVE_TYPE, LOBBY_MOVE_TYPE, GameMoveType, Message, LobbyGame, SERVER_EVENT_TYPE, PersonalGame, OtherPlayer, PrivateMessage, GameMessage } from './common';
import express from 'express'
import { create } from './create';
import { login } from './login';
import { join } from './join';
import { start } from './start';

interface GameMap {
    [key: string]: Game;
}

interface PlayerGameMap {
    [key: string]: string[];
}


// game_id -> players: [players], state: {}, status
const games: GameMap = {};

// also players can have multiple game so
const player_games: PlayerGameMap = {};


interface UserMap {
    [key: string]: User;
}

// strictly local. I think
interface UserPort {
    [key: string]: WebSocket;
}
const user_ports: UserPort = {};

const users: UserMap = {};

const PORT = 3000;
const WS_PORT = 3001;

// Create HTTP server
const server = http.createServer((req: http.IncomingMessage, res: http.ServerResponse) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Foolish Card Game Server Running\n');
});


// index really
const name_to_id: { [key: string]: string } = {};

const app = express()
app.use(express.json());

app.use((req, res, next) => {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS, PUT, PATCH, DELETE');
    res.setHeader('Access-Control-Allow-Headers', 'X-Requested-With, Content-Type, Accept');
    res.setHeader('Content-Type', 'application/json');
    next();
});

app.get('/', (req, res) => {
    res.send('Hello World')
})


const other_player = (player: Player): OtherPlayer => {
    return { name: player.name, id: player.id, hand_length: player.hand.length, status: player.status === PLAYER_STATUS.AWAITING_ATTACK ? PLAYER_STATUS.IN : player.status };
}

const personalize_game = (game: Game, player_id: string): PersonalGame => {
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

app.post('/' + LOBBY_MOVE_TYPE.LOGIN, login);

app.post('/' + LOBBY_MOVE_TYPE.CREATE, create);

app.post('/' + LOBBY_MOVE_TYPE.JOIN, join);

app.post('/' + LOBBY_MOVE_TYPE.START, start);

app.post('/' + GAME_MOVE_TYPE.STATUS, wrap400((req, res) => {
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))

app.post('/' + GAME_MOVE_TYPE.ATTACK, wrap400((req, res) => {
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_attack(games[game_id], game_id, player_id, req.body.cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))

app.post('/' + GAME_MOVE_TYPE.PASS, wrap400((req, res) => {
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_pass(games[game_id], game_id, player_id, req.body.cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))

app.post('/' + GAME_MOVE_TYPE.PICKUP, wrap400((req, res) => {
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_pickup(games[game_id], game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))

app.post('/' + GAME_MOVE_TYPE.COVER, wrap400((req, res) => {
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_cover(games[game_id], game_id, player_id, req.body.cover_cards, req.body.attack_cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))

app.post('/' + GAME_MOVE_TYPE.GOOD, wrap400((req, res) => { 
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_good(games[game_id], game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
}))
app.listen(3009)



// Create WebSocket server
const wss = new WebSocket.Server({ port: WS_PORT });

console.log(`HTTP Server running on http://localhost:${PORT}`);
console.log(`WebSocket Server running on ws://localhost:${WS_PORT}`);

const get_next_player_index = (game: Game, current_player: number): number => {
    let next_player = (current_player + 1) % game.players.length;
    while (game.players[next_player].status === PLAYER_STATUS.OUT) {
        next_player = (next_player + 1) % game.players.length;
    }
    return next_player;
}

const card_comp = (card1: Card, card2: Card): boolean => {
    return card1.suit === card2.suit && card1.value === card2.value;
}

const no_cards_left = (game: Game) => {
    return game.deck.length === 0 && game.flipped === null;
}

const game_done = (game: Game): string | null => {
    // only one 1 left, every0one else is out
    const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
    const out_players = game.players.filter(player => player.status === PLAYER_STATUS.OUT);
    if (in_players.length === 1 && out_players.length === game.players.length - 1) {
        return in_players[0].id;
    }
    return null;
}

const check_win = (game_id: string) => {
    const game = games[game_id];
    const the_fool = game_done(game);
    if (the_fool !== null) {
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'game_done',
                message: `Game done. Player ${the_fool} ends up the fool`
            }
        });
        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: Player) => {
            player.status = PLAYER_STATUS.IDLE;
            player.hand = [];
        });
        game.table = [];
        game.deck = refill_deck();
    }
}

// different from the one in index.ts because we do this BEFORE shifting positions
// hope it works
const refill = (game_id: string) => {
    const game = games[game_id];

    if (no_cards_left(game)) {
        return;
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if currently Attacked cleared their hand
    let defenseHand = game.players[game.currentlyAttacked].hand;
    if (defenseHand.length === 0) {
        // they draw first
        let cards_drawn = 0;
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcast_to_game(game_id, {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        broadcast_to_game(game_id, {
            type: 'player_refilled',
            message: `Player ${game.players[game.currentlyAttacked].name} refilled their empty hand with ${cards_drawn} cards`,
            cards: defenseHand
        });
    }

    // Then go around starting from firstAttacker
    let pIndex = game.firstAttacker;
    do {
        const hand = game.players[pIndex].hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcast_to_game(game_id, {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            broadcast_to_game(game_id, {
                type: 'player_refilled',
                message: `Player ${game.players[pIndex].name} drew ${cards_drawn} cards`,
                cards: hand
            });
        } else if (cards_drawn === 0 && game.players[pIndex].hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                broadcast_to_game(game_id, {
                    type: 'player_wins',
                    message: `Player ${game.players[pIndex].name} got rid of all their cards`
                });
                game.players[pIndex].status = PLAYER_STATUS.OUT;
                check_win(game_id);
            }
        }
        pIndex = get_next_player_index(game, pIndex);
        //pIndex = (pIndex + 1) % game.players.length;
    } while (pIndex !== game.firstAttacker/* && !no_cards_left(game)*/);
};


const verify_hands_in_players_hand = (player: Player, cards: Card[]) => {
    for (const card of cards) {
        if (!player.hand.some(handCard => card_comp(handCard, card))) {
            throw new Error(`Card ${cardDisplay(card)} is not in player ${player.id}'s hand`);
        }
    }
}

// Helper method to validate player is/isn't defender
const validate_defender_status = (game: Game, player_id: string, should_be_defender: boolean) => {
    const isDefender = game.players[game.currentlyAttacked].id === player_id;
    if (isDefender !== should_be_defender) {
        throw new Error(`Player ${player_id} is ${should_be_defender ? 'not' : ''} the defender`);
    }
}

const handle_pickup = (game: Game, game_id: string, player_id: string) => {

    // pick up a card

    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);
    // TODO add a timer + check to make sure they don't pick up too quickly

    // check if there are cards on the table
    if (game.table.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // ok let's just pick it up

    // add cards from table to hand
    game.table.forEach(battle => {
        game.players[game.currentlyAttacked].hand.push(battle.attack);
        if (battle.defense) {
            game.players[game.currentlyAttacked].hand.push(battle.defense);
        }
    });


    // clear table
    game.table = [];

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PICKUP_PLAYED,
            message: `Player ${player_id} picked up cards`,
            game: game
        }
    });

    // Draw cards starting from first attacker

    refill(game_id);

    // shift
    game.firstAttacker = get_next_player_index(game, game.currentlyAttacked);
    game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
}

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
    verify_hands_in_players_hand(game.players[game.currentlyAttacked], cover_cards);

    // check no duplicates
    if (new Set(cover_cards).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }


    // ensure that each of the attack cards are on the table AND uncovered
    for (const card of attack_cards) {
        if (!game.table.some(battle => battle.attack.value === card.value && battle.defense === null)) {
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
        if (!canCover(attack_card, cover_card, game.powerSuit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }

    // assert same size of arrays
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }

    // now cover the cards
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        // find the attack card on the table
        const attack_card_index = game.table.findIndex(battle => card_comp(battle.attack, attack_card) && battle.defense === null);
        if (attack_card_index === -1) {
            // This shouldn't happen as we just validated
            throw new Error('SEVERE: Card not found on table');
        }
        game.table[attack_card_index].defense = cover_card;
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.COVER_PLAYED,
                message: `Player ${player_id} covered ${cardDisplay(attack_card)} with ${cardDisplay(cover_card)}`,
                game: game
            }
        });
        // remove the cards from the hand
        //game.players[game.currentlyAttacked].hand = game.players[game.currentlyAttacked].hand.filter(card => !card_comp(card, cover_card));

    }

    // remove the cards from the hand
    game.players[game.currentlyAttacked].hand = game.players[game.currentlyAttacked].hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // There is one scenario where we instantly move on: the player has no cards left in their hand
    if (game.players[game.currentlyAttacked].hand.length === 0) {

        game.table = []; // burn the cards. TODO keep track of HOW MANY cards are burned but not which
        refill(game_id);
        // and it's fucking tricky because they can win here
        // shift 
        game.firstAttacker = game.currentlyAttacked;
        if (game.players[game.firstAttacker].hand.length === 0) {
            // can't think right now, but we need better win checking 
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: SERVER_EVENT_TYPE.PLAYER_WON,
                    message: `Player ${game.players[game.firstAttacker].name} got rid of their hand`,
                    game: game
                }
            });
            // win if still empty after refill
            game.players[game.firstAttacker].status = PLAYER_STATUS.OUT;
            check_win(game_id);
            game.firstAttacker = get_next_player_index(game, game.firstAttacker);
        }
        game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
        return;
    }

    // only do this if all table cards are covered but the defender has cards left
    // we know they have cards left
    const all_attacks_covered = game.table.every(battle => battle.defense !== null);
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
        for (const battle of game.table) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        // now we need to see who can play cards. not the defender lol
        const playable_players = game.players.filter(player => player.id !== game.players[game.currentlyAttacked].id && player.hand.some(card => playable_values.has(card.value)));
        if (playable_players.length === 0) {
            // no one can play cards
            // but don't make it that obvious. give it 30 seconds
            setTimeout(() => {
                //shift 
                public_game_channel.push({
                    game_id: game_id,
                    message: {
                        type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
                        message: `Player ${player_id} successfully defended the attack`,
                        game: game
                    }
                });
                game.table = [];
                refill(game_id);
                game.firstAttacker = game.currentlyAttacked;
                game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
                game.status = GAME_STATUS.FIRST_ATTACKER;
            }, 5000 + Math.random() * 20000);
        } else {
            // someone can play cards
            // so we need to see who can play cards
            playable_players.forEach(player => {
                player.status = PLAYER_STATUS.AWAITING_ATTACK;
            });

            playable_players.forEach(player => {
                private_user_channel.push({
                    user_id: player.id,
                    message: {
                        type: SERVER_EVENT_TYPE.PLAYABLE_CARDS,
                        message: `You can still play cards. Either play or confirm you are done attacking with "good"`,
                        game: game
                    }
                });
                // send them a message
                /*user_ports[player.id].send(JSON.stringify({
                    type: 'playable_cards',
                    message: `You can still play cards. Either play or confirm you are done attacking with "good"`
                }));*/
            });

        }
    }

}

const handle_good = (game: Game, game_id: string, player_id: string) => {

    // player is done attacking
    // we need to check if they have any cards left in their hand

    if (game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game_id} is not in wait_for_attackers mode`);
    }
    const player = game.players.find(player => player.id === player_id)!;
    // If they're in but can't play cards, just let them proceed
    if (player.status !== PLAYER_STATUS.IN && player.status !== PLAYER_STATUS.AWAITING_ATTACK) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // set them to done attacking
    player.status = PLAYER_STATUS.IN;

    // ok now we need to check if all players are done attacking
    // dont count the defender
    // the status check is critical
    const playable_players = game.players.filter(player => player.id !== game.players[game.currentlyAttacked].id && player.hand.some(card => card.value === game.flipped!.value) && player.status === PLAYER_STATUS.AWAITING_ATTACK);
    if (playable_players.length !== 0) {
        return;
    }

    // we are done attacking.
    // this has to be after a successful cover. Otherwise we'd still be waiting on the defender
    // shift
    // change all done_attacking to in
    game.players.forEach(player => {
        if (player.status === PLAYER_STATUS.AWAITING_ATTACK) {
            player.status = PLAYER_STATUS.IN;
        }
    });

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
            message: `Player ${player_id} successfully defended the attack`,
            game: game
        }
    });

    game.table = [];
    refill(game_id);

    //shift 
    game.firstAttacker = game.currentlyAttacked;
    game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
}

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
    if (game.table.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability. 1. no cover, 2. all same value, 3. next player has enough cards
    // 1. no cover
    if (game.table.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }
    // this also implies all same value on the table
    // and we already know that the pass cards are the same value
    // so check first pass card against all other cards on the tableo
    if (!game.table.every(battle => battle.attack.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.currentlyAttacked);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < mCards.length + game.table.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${mCards.map(card => cardDisplay(card)).join(', ')}`);
    }

    // Now we can pass
    // add to table
    //remove from hand
    // update currentlyAttacked

    for (const card of mCards) {
        game.table.push({
            attack: card,
            defense: null
        });
    }
    player.hand = player.hand.filter(card => !mCards.some(mCard => card_comp(card, mCard)));


    // If the deck is empty, they can get out here
    if (no_cards_left(game) && player.hand.length === 0) {
        // they win
        player.status = PLAYER_STATUS.OUT;
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'player_wins',
                message: `Player ${player_id} got rid of all their cards`,
                game: game
            }
        });
        /*broadcast_to_game(game_id, {
            type: 'player_wins',
            message: `Player ${player_id} got rid of all their cards`
        });*/
        check_win(game_id);
    }

    game.currentlyAttacked = next_player_index;


    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PASS_PLAYED,
            message: `Player ${player_id} used ${mCards.map(card => cardDisplay(card)).join(', ')} to pass to ${next_player.name}`,
            cards: mCards,
            game: game
        }
    });

    const uncovered_cards = game.table.filter(battle => battle.defense === null).length;
    const defender_cards = game.players[game.currentlyAttacked].hand.length;

    // it's important to check if we need to shift to only_defend
    if (uncovered_cards === defender_cards) {
        // just reached the limit
        game.status = 'only_defend';
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'no_more_attacks',
                message: `Maximum number of attacks reached, only defender can defend`,
                game: game
            }
        });
    } else if (uncovered_cards > defender_cards) {
        // how the fuck did this happen
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        // a pass could shift from only_defend to free_play
        game.status = 'free_play';
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'free_play_mode',
                message: `Passed cards, now free play mode`,
                game: game
            }
        });
    }
}

const handle_attack = (game: Game, game_id: string, player_id: string, cards: Card[]) => {
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
    const player = game.players.find(player => player.id === player_id)!;

    // also the attacker cannot be the defender
    validate_defender_status(game, player_id, false);

    // check if every card is in hand
    verify_hands_in_players_hand(player, cards);

    // make sure there are enough cards in the defenders hand
    let uncovered_cards = game.table.filter(battle => battle.defense === null).length;
    let defender_cards = game.players[game.currentlyAttacked].hand.length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error(`Player ${player_id} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // check if player is first attacker
        if (game.players[game.firstAttacker].id !== player.id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }

        // Ok passed checks, we can put the cards on the table
        // remove from hand, put on table
        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));

        for (const card of cards) {
            game.table.push({
                attack: card,
                defense: null
            });
        }

        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
                game: game
            }
        });

        // It's possible they win here
        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'player_wins',
                    message: `Player ${player_id} got rid of all their cards`
                }
            });
            check_win(game_id);
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
        if (!cards.every(card => game.table.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
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
            game.table.push({
                attack: card,
                defense: null
            });
        }

        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
                game: game
            }
        });

        // It's possible they win here
        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'player_wins',
                    message: `Player ${player_id} got rid of all their cards`
                }
            });
            check_win(game_id);
        }

        uncovered_cards = game.table.filter(battle => battle.defense === null).length;
        defender_cards = game.players[game.currentlyAttacked].hand.length;

        // it's important to check if we need to shift to only_defend
        if (uncovered_cards === defender_cards) {
            // just reached the limit
            game.status = GAME_STATUS.ONLY_DEFEND;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'no_more_attacks',
                    message: `Maximum number of attacks reached, only defender can defend`
                }
            });
        } else if (uncovered_cards > defender_cards) {
            // how the fuck did this happen
            throw new Error('SEVERE: Uncovered cards > defender_cards');
        }


    } else if (game.status === GAME_STATUS.ONLY_DEFEND) {
        // just reject
        throw new Error(`Player ${player_id} tried to attack but game is in only_defend mode`);
    } else {
        // handle others later
    }
}



// This will emulate one of the realtime channels of supabase. Most server events will go here
const public_game_channel: GameMessage[] = [];

// I think just "request good" and "draws _ card" will be here
const private_user_channel: PrivateMessage[] = [];

wss.on('connection', (ws: WebSocket) => {
    console.log('New client connected');

    // Give the client a unique id
    //const player_id = createId();
    //users[player_id] = {
    //name: '',
    //id: player_id
    ////}
    //user_ports[player_id] = ws;

    // Send welcome message to client
    const welcomeMessage: Message = {
        type: 'welcome',
        message: 'Connected to Foolish Card Game Server'
    };
    ws.send(JSON.stringify(welcomeMessage));

    // Handle messages from client
    ws.on('message', (data: WebSocket.Data) => {
        try {
            const message: Message = JSON.parse(data.toString());
            console.log('Received from client:', message);
            if (message.type === LOBBY_MOVE_TYPE.WEBSOCKET_CONNECT) {
                const player_id = message.player_id!;
                // Really the critical part here
                user_ports[player_id] = ws;
            } else {
                throw new Error(`Unknown message type: ${message.type}`);
            }

        } catch (error) {
            // This is the only type we actually care about lol
            ws.send(JSON.stringify({
                type: 'error',
                message: `Error: ${error}`
            }));
            console.error('Error parsing message:', error);
        }
    });

    // Handle client disconnect
    ws.on('close', () => {
        console.log('Client disconnected');
        // remove from user_ports
        // find where in user_ports this ws is
        const player_id = Object.keys(user_ports).find(id => user_ports[id] === ws);
        if (player_id) {
            delete user_ports[player_id];
        }
    });

    // Handle errors
    ws.on('error', (error: Error) => {
        console.error('WebSocket error:', error);
    });
});

// Start HTTP server
server.listen(PORT, () => {
    console.log(`Server is listening on port ${PORT}`);
});

// Handle server shutdown gracefully
process.on('SIGINT', () => {
    console.log('\nShutting down server...');
    server.close();
    wss.close();
    process.exit(0);
});


const broadcast_to_game = (game_id: string, message: Message) => {
    games[game_id].players.forEach(player => {
        user_ports[player.id].send(JSON.stringify(message));
    });
}

// Speed up or down for the hell of it
const SERVER_LOOP_INTERVAL = 1000;

setInterval(() => {
    // Batch to every 10s?

    // assuming they are kinda threadsafe lol
    while (public_game_channel.length > 0) {
        const message = public_game_channel.shift();
        if (message) {
            const game_id = message.game_id;
            if (games[game_id].status === GAME_STATUS.WAITING) {
                // we need to send the game to the client
                message.message.game = lobbify_game(games[game_id]);
                games[game_id].players.forEach(player => {
                    const port = user_ports[player.id];
                    if (port && port.readyState === WebSocket.OPEN) {
                        port.send(JSON.stringify(message));
                    }
                });
            } else {
                // we need to send the game to the client
                games[game_id].players.forEach(player => {
                    message.message.game = personalize_game(games[game_id], player.id);
                    const port = user_ports[player.id];
                    if (port && port.readyState === WebSocket.OPEN) {
                        port.send(JSON.stringify(message));
                    }
                });
            }
        }
    }

    while (private_user_channel.length > 0) {
        const message = private_user_channel.shift();
        if (message) {
            const port = user_ports[message.user_id];
            message.message.game = personalize_game(message.message.game as Game, message.user_id);
            if (port && port.readyState === WebSocket.OPEN) {
                port.send(JSON.stringify(message));
            }
        }
    }

}, SERVER_LOOP_INTERVAL);