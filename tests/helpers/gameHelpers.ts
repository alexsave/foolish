import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, Battle } from '../../supabase/functions/_shared/types';

// Helper to create a mock card
export const createMockCard = (suit: number, value: number): Card => ({
  suit,
  value,
});

// Helper to create a mock private player
export const createMockPlayer = (
  player_id: string,
  name: string,
  hand: Card[] = [],
  status: typeof PLAYER_STATUS[keyof typeof PLAYER_STATUS] = PLAYER_STATUS.IN,
  is_ai: boolean = false
): PrivatePlayer => ({
  player_id,
  name,
  hand,
  status,
  hand_length: hand.length,
  is_ai,
  awaiting_attack: false,
  done_attacking_this_round: false,
});

// Helper to create a mock battle
export const createMockBattle = (attack: Card, defense: Card | null = null): Battle => ({
  attack,
  defense,
});

// Helper to create a basic mock game
export const createMockGame = (
  id: string = 'test-game-id',
  status: typeof GAME_STATUS[keyof typeof GAME_STATUS] = GAME_STATUS.FREE_PLAY,
  players: PrivatePlayer[] = [],
  first_attacker: number = 0,
  defender: number = 1,
  power_suit: number = 1,
  table_battles: Battle[] = [],
  deck: Card[] = []
): Game => ({
  id,
  name: 'Test Game',
  deck,
  deck_length: deck.length,
  flipped: deck.length > 0 ? deck[0] : null,
  players,
  status,
  power_suit,
  first_attacker,
  defender,
  table_battles,
  elimination_order: [],
});

// Helper to create a standard 2-player game setup
export const createTwoPlayerGame = (
  gameStatus: typeof GAME_STATUS[keyof typeof GAME_STATUS] = GAME_STATUS.FREE_PLAY,
  attackerHand: Card[] = [],
  defenderHand: Card[] = [],
  table_battles: Battle[] = []
): Game => {
  const attacker = createMockPlayer('attacker-id', 'Attacker', attackerHand);
  const defender = createMockPlayer('defender-id', 'Defender', defenderHand);
  
  return createMockGame(
    'test-game-id',
    gameStatus,
    [attacker, defender],
    0, // first_attacker index
    1, // defender index
    1, // power_suit (spades)
    table_battles,
    [] // empty deck for simplicity
  );
};

// Helper to create a 3-player game setup
export const createThreePlayerGame = (
  gameStatus: typeof GAME_STATUS[keyof typeof GAME_STATUS] = GAME_STATUS.FREE_PLAY,
  hands: Card[][] = [[], [], []],
  table_battles: Battle[] = []
): Game => {
  const players = [
    createMockPlayer('player1-id', 'Player 1', hands[0]),
    createMockPlayer('player2-id', 'Player 2', hands[1]),
    createMockPlayer('player3-id', 'Player 3', hands[2]),
  ];
  
  return createMockGame(
    'test-game-id',
    gameStatus,
    players,
    0, // first_attacker index
    1, // defender index
    1, // power_suit (spades)
    table_battles,
    [] // empty deck for simplicity
  );
};

// Common card sets for testing
// Note: Using correct suit/value mappings to match cardDisplay format
// SUIT_MAP: [0=Spades, 1=Hearts, 2=Clubs, 3=Diamonds]
// VALUE_MAP: [null, '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A']
export const COMMON_CARDS = {
  // Basic cards (Hearts = suit 1)
  SIX_HEARTS: createMockCard(1, 5),   // "6 of Hearts"
  SEVEN_HEARTS: createMockCard(1, 6), // "7 of Hearts"
  EIGHT_HEARTS: createMockCard(1, 7), // "8 of Hearts"
  NINE_HEARTS: createMockCard(1, 8),  // "9 of Hearts"
  TEN_HEARTS: createMockCard(1, 9),   // "10 of Hearts"
  JACK_HEARTS: createMockCard(1, 10), // "J of Hearts"
  QUEEN_HEARTS: createMockCard(1, 11), // "Q of Hearts"
  KING_HEARTS: createMockCard(1, 12),  // "K of Hearts"
  ACE_HEARTS: createMockCard(1, 13),   // "A of Hearts"
  
  // Spades (power suit = suit 0)
  SIX_SPADES: createMockCard(0, 5),   // "6 of Spades"
  SEVEN_SPADES: createMockCard(0, 6), // "7 of Spades"
  EIGHT_SPADES: createMockCard(0, 7), // "8 of Spades"
  NINE_SPADES: createMockCard(0, 8),  // "9 of Spades"
  TEN_SPADES: createMockCard(0, 9),   // "10 of Spades"
  JACK_SPADES: createMockCard(0, 10), // "J of Spades"
  QUEEN_SPADES: createMockCard(0, 11), // "Q of Spades"
  KING_SPADES: createMockCard(0, 12),  // "K of Spades"
  ACE_SPADES: createMockCard(0, 13),   // "A of Spades"
  
  // Diamonds (suit 3)
  SIX_DIAMONDS: createMockCard(3, 5),   // "6 of Diamonds"
  SEVEN_DIAMONDS: createMockCard(3, 6), // "7 of Diamonds"
  EIGHT_DIAMONDS: createMockCard(3, 7), // "8 of Diamonds"
  NINE_DIAMONDS: createMockCard(3, 8),  // "9 of Diamonds"
  TEN_DIAMONDS: createMockCard(3, 9),   // "10 of Diamonds"
  
  // Clubs (suit 2)
  SIX_CLUBS: createMockCard(2, 5),   // "6 of Clubs"
  SEVEN_CLUBS: createMockCard(2, 6), // "7 of Clubs"
  EIGHT_CLUBS: createMockCard(2, 7), // "8 of Clubs"
  NINE_CLUBS: createMockCard(2, 8),  // "9 of Clubs"
  TEN_CLUBS: createMockCard(2, 9),   // "10 of Clubs"
};

// Helper to create a deck with specific cards
export const createDeck = (cards: Card[]): Card[] => [...cards];

// Helper to create a standard testing deck
export const createStandardTestDeck = (): Card[] => [
  ...Object.values(COMMON_CARDS),
];

// Helper to expect error messages
export const expectError = (fn: () => void, expectedMessage: string): void => {
  expect(fn).toThrow(expectedMessage);
};

// Helper to expect async error messages
export const expectAsyncError = async (fn: () => Promise<void>, expectedMessage: string): Promise<void> => {
  await expect(fn).rejects.toThrow(expectedMessage);
}; 