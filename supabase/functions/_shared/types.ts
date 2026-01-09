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
    ATTACK: 'attack',
    WAIT: 'wait'
} as const;

export type GameMoveType = typeof GAME_MOVE_TYPE[keyof typeof GAME_MOVE_TYPE];

export const SERVER_EVENT_TYPE = {
    PLAYER_JOINED_GAME: 'player_joined_game',
    PLAYER_LEFT_GAME: 'player_left_game',
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

export const ANIMATION_EVENT_TYPE = {
    MAGIC_TRANSITION: 'magic_transition', // change online view
    DEAL: 'deal', // deck -> hand
    FLIPPED: 'flipped', // deck -> flipped
    DEFENDER_MOVE: 'defender_move', // defender shall move
    ATTACK_PASS: 'attack_pass', // hand -> table
    COVER: 'cover', // hand -> specific card
    PICKUP: 'pickup', // hand -> table -> hand
    DISCARD: 'discard', // table -> garbage
    OUT: 'out', // dim the name
    REFILL: 'refill', // cards to hand
    CARDS_TO_TRASH: 'cards_to_trash' // cards to discard pile
} as const;

export type AnimationEventType = typeof ANIMATION_EVENT_TYPE[keyof typeof ANIMATION_EVENT_TYPE];

// Base animation event for public view (spectators)
export interface PublicAnimationEvent {
    type: AnimationEventType;
    player_id?: string;
    cards?: Card[]; // May contain card backs (-1, -1) for hidden cards
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card; // for cover events
    battle_index?: number; // for cover events
    message?: string;
    game_state: PublicGame; // public game state
}

// Personal animation event for individual players
export interface PersonalAnimationEvent extends PublicAnimationEvent {
    game_state: PersonalGame; // personalized game state with self data
}

// Full animation event with all private data (server-side only)
export interface AnimationEvent extends PublicAnimationEvent {
    game_state: Game; // full game state with all private data
}

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
    strategy_key: string; // strategy key for bots so we don't need to load the bot table too. just "human" for players
}

// base game type
export interface PublicGame {
    id: string;
    name: string;
    deck_length: number;
    discard_pile_length: number;
    flipped: Card | null;
    players: PublicPlayer[];
    status: GameStatus;
    power_suit: number;
    first_attacker: number;
    defender: number;
    table_battles: Battle[];
    elimination_order: string[]; // Array of player_ids in order they were eliminated
    good_timestamp: number | null; // Timestamp when all attacks were covered, null if not all covered
    good_players: string[]; // Array of player_ids who have pressed 'good'
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
    logs: GameLog[]; // Pending logs to be saved with game state
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
}

// Log types for bot memory and game history
export const STRATEGY_KEY = {
    HUMAN: 'human',
    RANDOM: 'random',
    HANDWRITTEN: 'handwritten',
    SIMPLE_HEURISTIC: 'simple_heuristic',
    ULTIMATE_CHAMPION: 'ultimate_champion',
    CHAMPION: 'champion',
    HACKER: 'hacker',
    CONSOLE: 'console',
    GPT: 'gpt'
} as const;

export type StrategyKey = typeof STRATEGY_KEY[keyof typeof STRATEGY_KEY];

export const LOG_TYPE = {
    GAME_START: 'game_start',
    ATTACK: 'attack',
    COVER: 'cover',
    PASS: 'pass',
    PICKUP: 'pickup',
    GOOD: 'good',
    DISCARD: 'discard',
    DEFENDER_CHANGE: 'defender_change',
    PLAYER_OUT: 'player_out',
    DRAW: 'draw' // Player draws N cards from deck (unknown cards are {suit:-1, value:-1})
} as const;

export type LogType = typeof LOG_TYPE[keyof typeof LOG_TYPE];

// Card pair for logs - used to track card relationships
export interface LogCardPair {
    primary: Card; // The main card (attack card, pass card, pickup card, cover card, etc.)
    target?: Card | null; // Only used for COVER events - the attack card being covered
}

// Unsaved log entry (before database insertion)
export interface UnsavedGameLog {
    game_id: string;
    log_type: LogType;
    player_id: string | null; // Player who performed the action (null for system events like discard/defender_change)
    card_pairs: LogCardPair[]; // Array of {primary, target} pairs
    defender_index: number | null; // For defender_change events, the new defender index
}

// Saved log entry from database (with id and created_at)
export interface GameLog extends UnsavedGameLog {
    id: string; // UUID, generated by database
    created_at: string; // Timestamp, generated by database
}