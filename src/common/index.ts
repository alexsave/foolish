// Start with
let startTime = performance.now();
// The engine of fools
// I had a whole paper on this but let's just get started

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