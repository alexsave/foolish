// Types and interfaces for the game

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

export interface ChatMessage {
    is_system: boolean;
    game_id: string;
    user_id: string;
    message: string;
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

// Stripped down versions
export interface LobbyPlayer {
    name: string;
    status: PlayerStatus;
    id: string;
}

export interface LobbyGame {
    id: string;
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
    id: string;
    deck_length: number;
    flipped: Card | null;
    self: Player;
    players: OtherPlayer[];

    // wait for attackers reveals that people do have hands, so we don't allow this
    // eh it does but there's no way around this. if no one has hands, best we can do is keep status as waitforattackers for a bit
    status: GameStatus;
    power_suit: number;
    first_attacker: number;
    currently_attacked: number;
    previous_first_attacker: number;
    previous_currently_attacked: number;
    table_battles: Battle[];
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
    id: string;
    deck: Card[];
    flipped: Card | null;
    players: Player[];
    status: 'waiting' | 'playing' | 'first_attacker' | 'free_play' | 'only_defend' | 'wait_for_attackers';
    power_suit: number;
    first_attacker: number;
    currently_attacked: number;
    previous_first_attacker: number;
    previous_currently_attacked: number;
    table_battles: Battle[];
}

export interface Battle {
    attack: Card;
    defense: Card | null;
}

// Internal interfaces
export interface CardListMapping {
    [key: string]: Card[];
}

export interface CardMap {
    [key: string]: Card;
}

export interface Move {
    type: 'pass' | 'throw' | 'pickup' | 'cover' | 'success';
    player: string;
    card?: Card;
    coverMap?: Map<Card, Card>;
} 