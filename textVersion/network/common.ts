// In the actual game, we will have to have a script to copy this from supabase/ to src/
import express from 'express';
import WebSocket from 'ws';
import { refill_deck, initialize_hands, draw, cardDisplay, determine_lowest_power_index, set_positions } from './index';

// user table of id to name
export interface User {
    name: string;
    id: string;
}

export interface GameMessage {
    game_id: string;
    message: Message;
}

export interface PrivateMessage {
    user_id: string;
    message: Message;
}

export const PLAYER_STATUS = {
    IDLE: 'idle',
    READY: 'ready',
    IN: 'in',
    OUT: 'out',
    // players that need to confirm "good" will be put in this state
    AWAITING_ATTACK: 'awaiting_attack' 
} as const;

export type PlayerStatus = typeof PLAYER_STATUS[keyof typeof PLAYER_STATUS];

export const GAME_STATUS = {
    WAITING: 'waiting',
    PLAYING: 'playing',
    FIRST_ATTACKER: 'first_attacker',
    FREE_PLAY: 'free_play',
    ONLY_DEFEND: 'only_defend',
    WAIT_FOR_ATTACKERS: 'wait_for_attackers'
} as const;

export type GameStatus = typeof GAME_STATUS[keyof typeof GAME_STATUS];

// Stripped down versions
export interface LobbyPlayer {
    name: string;
    status: PlayerStatus;
    id: string;
}

export interface LobbyGame {
    players: LobbyPlayer[];
    status: GameStatus;
}

export interface OtherPlayer {
    id: string;
    name: string;
    hand_length: number;
    // TODO IMPORTANT: when we get status, we need to map done_attacking to in to avoid revealing values
    status: 'idle' | 'ready' | 'in' | 'out';
}

// personal game is what gets sent to clients. they do not see other players hands, only length
export interface PersonalGame {
    deck_length: number;
    flipped: Card | null;
    self: Player;
    players: OtherPlayer[];

    // wait for attackers reveals that people do have hands, so we don't allow this
    // eh it does but there's no way around this. if no one has hands, best we can do is keep status as waitforattackers for a bit
    status: GameStatus;
    powerSuit: number;
    firstAttacker: number;
    currentlyAttacked: number;
    previousFirstAttacker: number;
    previousCurrentlyAttacked: number;
    table: Battle[];
}

export interface Message {
    type: string;
    message: string;
    timestamp?: string;
    game_id?: string;
    cards?: Card[];
    attack_cards?: Card[];// cards that will be covered
    player_name?: string;
    player_id?: string;
    game?: Game | LobbyGame | PersonalGame;
}

export const LOBBY_MOVE_TYPE = {
    CREATE: 'create',
    JOIN: 'join',
    BACK: 'back',
    START: 'start',
    ENTER: 'enter',
    LIST: 'list',
    LOGIN: 'login',
    WEBSOCKET_CONNECT: 'websocket_connect'
} as const;
export type LobbyMoveType = typeof LOBBY_MOVE_TYPE[keyof typeof LOBBY_MOVE_TYPE];

export const GAME_MOVE_TYPE = {
    PASS: 'pass',
    THROW: 'throw',
    PICKUP: 'pickup',
    COVER: 'cover',
    SUCCESS: 'success',
    STATUS: 'status',
    GOOD: 'good',
    ATTACK: 'attack'
} as const;

export type GameMoveType = typeof GAME_MOVE_TYPE[keyof typeof GAME_MOVE_TYPE];


// Interfaces
export interface Card {
    suit: number;
    value: number;
}

export interface Player {
    id: string;
    name: string;
    hand: Card[];
    // TODO IMPORTANT: when we get status, we need to map done_attacking to in to avoid revealing values
    status: PlayerStatus;
}
export interface Game {
    deck: Card[];
    flipped: Card | null;
    players: Player[];
    status: 'waiting' | 'playing' | 'first_attacker' | 'free_play' | 'only_defend' | 'wait_for_attackers';
    powerSuit: number;
    firstAttacker: number;
    currentlyAttacked: number;
    previousFirstAttacker: number;
    previousCurrentlyAttacked: number;
    table: Battle[];
}
interface CardListMapping {
    [key: string]: Card[];
}
interface CardMap {
    [key: string]: Card;
}
// First check if all can be covered
// This is actually a bit tricky becuase there can be mulitple options, some better than others
interface Move {
    type: 'pass' | 'throw' | 'pickup' | 'cover' | 'success';
    player: string;
    card?: Card;
    coverMap?: Map<Card, Card>;
}
interface Battle {
    attack: Card;
    defense: Card | null;
}

// Constants
export const CARDS_PER_PLAYER = 6;
const [SPADES, HEARTS, CLUBS, DIAMONDS] = [0, 1, 2, 3];
const SUITS = [SPADES, HEARTS, CLUBS, DIAMONDS];
const SUIT_MAP = ['Spades', 'Hearts', 'Clubs', 'Diamonds'];
const VALUE_MAP = [
    null, //0
    '2', //1
    '3', //2
    '4', //3
    '5', //4
    '6', //5
    '7', //6
    '8', //7
    '9', //8
    '10', //9
    'J', //10
    'Q', //11
    'K', //12
    'A', //13
];
const NAMES = ['Rando', 'Smarty', 'John', 'Blake', 'William', 'Zach', 'Alex', 'Ben', 'Caleb', 'Dylan', 'Ethan', 'Finn', 'Gavin', 'Hunter', 'Isaiah', 'Jack', 'Kyle', 'Landon', 'Mason', 'Nathan', 'Oliver', 'Parker', 'Quinn', 'Ryan', 'Samuel', 'Thomas', 'Ulysses', 'Vance', 'Wesley', 'Xavier', 'Yusuf', 'Zane']
export const ACE_VALUE = 13;
const CARDS_PER_SUIT = 5;
const START_VALUE = ACE_VALUE - CARDS_PER_SUIT + 1;
const PLAYER_COUNT = 7;

export interface Game {
    deck: Card[];
    flipped: Card | null;
    players: Player[];
    status: 'waiting' | 'playing' | 'first_attacker' | 'free_play' | 'only_defend' | 'wait_for_attackers';
    powerSuit: number;
    firstAttacker: number;
    currentlyAttacked: number;
    previousFirstAttacker: number;
    previousCurrentlyAttacked: number;
    table: Battle[];
}

export const SERVER_EVENT_TYPE = {
    PLAYER_JOINED_GAME: 'player_joined_game',
    PLAYER_READY: 'player_ready',
    GAME_STARTED: 'game_started',
    ATTACK_PLAYED: 'attack_played',
    PASS_PLAYED: 'pass_played',
    PICKUP_PLAYED: 'pickup_played',
    COVER_PLAYED: 'cover_played',
    PLAYER_WON: 'player_won',
    SUCCESSFULLY_COVERED: 'successfully_covered',
    PLAYABLE_CARDS: 'playable_cards'
} as const;

export type ServerEventType = typeof SERVER_EVENT_TYPE[keyof typeof SERVER_EVENT_TYPE];

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

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
        players: game.players.map(player => ({ name: player.name, status: player.status, id: player.id })),
        status: game.status === GAME_STATUS.WAITING ? GAME_STATUS.WAITING : GAME_STATUS.PLAYING
    };
}

// Basically the supabase schemas

export const database = {
    games: {} as Record<string, Game>,
    users: {} as Record<string, User>,
    player_games: {} as Record<string, string[]>,
    public_game_channel: [] as GameMessage[],
    private_user_channel: [] as PrivateMessage[],
    name_to_id: {} as Record<string, string>,
    user_ports: {} as Record<string, WebSocket>
}

// throw error if not found
export const verify_game_id = (game_id: string) => {
    const { games } = database;
    if (!games[game_id]) {
        throw new Error(`Game ${game_id} not found`);
    }
    return game_id;
}

export const verify_player_in_game = (game_id: string, player_id: string) => {
    const { player_games } = database;
    if (!player_games[player_id] || !player_games[player_id].includes(game_id)) {
        throw new Error(`Player ${player_id} is not in game ${game_id}`);
    }
}

export const start_game = (game_id: string) => {
    const { games, public_game_channel, private_user_channel } = database;

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