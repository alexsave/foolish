// Game constants

// Constants
export const CARDS_PER_PLAYER = 6;

// Suits
export const SPADES = 0;
export const HEARTS = 1;
export const CLUBS = 2;
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

export const NAMES = [
    'Rando', 'Smarty', 'John', 'Blake', 'William', 'Zach', 'Alex', 'Ben', 
    'Caleb', 'Dylan', 'Ethan', 'Finn', 'Gavin', 'Hunter', 'Isaiah', 'Jack', 
    'Kyle', 'Landon', 'Mason', 'Nathan', 'Oliver', 'Parker', 'Quinn', 'Ryan', 
    'Samuel', 'Thomas', 'Ulysses', 'Vance', 'Wesley', 'Xavier', 'Yusuf', 'Zane'
];

export const ACE_VALUE = 13;
export const CARDS_PER_SUIT = 9;
export const START_VALUE = ACE_VALUE - CARDS_PER_SUIT + 1;
export const PLAYER_COUNT = 7; 