// Types and interfaces for the game
// Player was id name status hand

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
    OUT: 'out'
} as const;

export type PlayerStatus = typeof PLAYER_STATUS[keyof typeof PLAYER_STATUS];

export const GAME_STATUS = {
    WAITING: 'waiting',
    PLAYING: 'playing',
    FIRST_ATTACKER: 'first_attacker',
    FREE_PLAY: 'free_play',
    ONLY_DEFEND: 'only_defend',
    WAIT_FOR_ATTACKERS: 'wait_for_attackers',
    GAME_OVER: 'game_over'
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
    PLAYABLE_CARDS: 'playable_cards',
    FIRST_ATTACKER: 'first_attacker',
    FLIPPED_CARD: 'flipped_card',
    GAME_NAME_UPDATED: 'game_name_updated',
    PLAYERS_REARRANGED: 'players_rearranged',
    HAND_REARRANGED: 'hand_rearranged',
    GOOD_PLAYED: 'good_played'
} as const;

export type ServerEventType = typeof SERVER_EVENT_TYPE[keyof typeof SERVER_EVENT_TYPE];

export const PRIVATE_EVENT_TYPE = {
    PLAYER_HAND: 'player_hand',
    REQUEST_FIRST_ATTACK: 'request_first_attack'
    //PLAYER_STATUS: 'player_status'
} as const;

export type PrivateEventType = typeof PRIVATE_EVENT_TYPE[keyof typeof PRIVATE_EVENT_TYPE];

// Stripped down versions
export interface LobbyPlayer {
    name: string;
    status: PlayerStatus;
    id: string;
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
    game?: Game | PersonalGame;
}

// Interfaces
export interface Card {
    suit: number;
    value: number;
}

export interface PublicPlayer {
    player_id: string;
    status: PlayerStatus;
    name: string;
    hand_length: number; // how to get this? ez. just keep it in sync
    is_ai: boolean; // true if this is an AI bot player
}

export interface PrivatePlayer extends PublicPlayer {
    hand: Card[];
    awaiting_attack: boolean; // private info stored in player_hands table
    done_attacking_this_round: boolean; // Flag to indicate bot is done attacking this round
}

// base game type
export interface PublicGame {
    id: string;
    name: string;
    deck_length: number;
    flipped: Card | null;
    players: PublicPlayer[];
    status: GameStatus;
    power_suit: number;
    first_attacker: number;
    defender: number;
    table_battles: Battle[];
    elimination_order: string[]; // Array of player_ids in order they were eliminated
}

// Personal game is what gets sent to clients. they do not see other players hands, only length
export interface PersonalGame extends PublicGame {
    self: PrivatePlayer;
}

// Full game for working with game logic
// Complete game with deck generated on-demand for game logic
export interface Game extends PublicGame {
    deck: Card[];
    players: PrivatePlayer[];
}

export interface Battle {
    attack: Card;
    defense: Card | null;
}

// What is actually in the database
export interface PlayerHand {
    game_id: string;
    player_id: string;
    hand: Card[];
    awaiting_attack: boolean;
}

export interface GameDeck {
    game_id: string;
    deck: Card[];
}

export interface UserEloRating {
    user_id: string;
    elo_rating: number;
    previous_elo: number;
    games_played: number;
    created_at: string;
    updated_at: string;
}

// Bot-related interfaces
export interface Bot {
    id: string;
    nickname: string;
    strategy_key: string;
    elo_rating: number;
    previous_elo: number;
    games_played: number;
    created_at: string;
    updated_at: string;
}

export interface BotHand {
    bot_id: string;
    hand: Card[];
    awaiting_attack: boolean;
    done_attacking_this_round: boolean;
}