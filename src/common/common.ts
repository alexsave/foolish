// In the actual game, we will have to have a script to copy this from supabase/ to src/
export const PLAYER_STATUS = {
    IDLE: 'idle',
    READY: 'ready',
    IN: 'in',
    OUT: 'out',
    DONE_ATTACKING: 'done_attacking'
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
}

export interface LobbyGame {
    players: LobbyPlayer[];
    status: 'waiting' | 'playing' | 'first_attacker' | 'free_play' | 'only_defend' | 'wait_for_attackers';
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
    game?: Game | LobbyGame;
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
    status: 'idle' | 'ready' | 'in' | 'done_attacking' |'out';
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
    GAME_STARTED: 'game_started'
} as const;

export type ServerEventType = typeof SERVER_EVENT_TYPE[keyof typeof SERVER_EVENT_TYPE];