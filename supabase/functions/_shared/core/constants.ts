// Game constants
export const CARDS_PER_PLAYER = 6;

// Suits
const SPADES = 0;
export const HEARTS = 1;
const CLUBS = 2;
export const DIAMONDS = 3;
export const SUITS = [SPADES, HEARTS, CLUBS, DIAMONDS];

export const SUIT_MAP = ['Spades', 'Hearts', 'Clubs', 'Diamonds'];

export const VALUE_MAP = [
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

export const ACE_VALUE = 13;
export const MAX_PLAYERS = 8;

// THE deck rule, settled (mirror of sdk/c/src/card.h min_value_for — the
// kernel is authoritative): 2..5 players play the 36-card deck (values
// 5..A), 6..8 players the full 52-card deck (2..A). Derive deck size and
// the lowest value from these helpers — never inline `n >= 6 ? 52 : 36`.
export const minValueFor = (numPlayers: number): number => (numPlayers >= 6 ? 1 : 5);
export const deckSizeFor = (numPlayers: number): number =>
    4 * (ACE_VALUE - minValueFor(numPlayers) + 1);