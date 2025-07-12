// Import pure utility functions from common_utils (no JSR dependencies)
import { 
  cardDisplay, 
  card_comp, 
  verify_cards_in_players_hand, 
  validate_defender_status, 
  no_cards_left, 
  draw, 
  refill_deck, 
  initialize_hands, 
  determine_lowest_power_index, 
  set_positions, 
  game_done
} from '../supabase/functions/_shared/common_utils';

// Import database-dependent functions from utils (with JSR dependencies)
import { 
  refill, 
  start_game, 
  check_win, 
  updateEloRatings, 
  getOrCreateEloRating, 
  getBotEloRating,
  verify_player_in_game,
  personalize_game
} from '../supabase/functions/_shared/utils';
import { createMockGame, createMockPlayer } from './helpers/gameHelpers';
import { Game, GAME_STATUS, PLAYER_STATUS, Card, PrivatePlayer } from '../supabase/functions/_shared/types';

// Mock only the database-dependent functions from utils.ts
jest.mock('../supabase/functions/_shared/utils', () => ({
  refill: jest.fn(),
  start_game: jest.fn(),
  check_win: jest.fn(),
  updateEloRatings: jest.fn(),
  getOrCreateEloRating: jest.fn(),
  getBotEloRating: jest.fn(),
  verify_player_in_game: jest.fn(),
  personalize_game: jest.fn(),
  executeWithGameLock: jest.fn((gameId: string, operation: () => Promise<any>) => operation()),
}));

describe('Utils Functions', () => {
  describe('cardDisplay', () => {
    it('should display card correctly', () => {
      const card: Card = { suit: 0, value: 13 };
      const result = cardDisplay(card);
      expect(result).toBe('A of Spades');
    });

    it('should handle different suits and values', () => {
      expect(cardDisplay({ suit: 1, value: 12 })).toBe('K of Hearts');
      expect(cardDisplay({ suit: 2, value: 5 })).toBe('6 of Clubs');
      expect(cardDisplay({ suit: 3, value: 10 })).toBe('J of Diamonds');
    });
  });

  describe('card_comp', () => {
    it('should return true for identical cards', () => {
      const card1: Card = { suit: 0, value: 13 };
      const card2: Card = { suit: 0, value: 13 };
      expect(card_comp(card1, card2)).toBe(true);
    });

    it('should return false for different cards', () => {
      const card1: Card = { suit: 0, value: 13 };
      const card2: Card = { suit: 1, value: 13 };
      expect(card_comp(card1, card2)).toBe(false);
      
      const card3: Card = { suit: 0, value: 12 };
      expect(card_comp(card1, card3)).toBe(false);
    });
  });

  describe('verify_player_in_game', () => {
    it('should be mocked (database-dependent function)', () => {
      const game = createMockGame();
      const player = createMockPlayer('player1', 'Player 1');
      game.players = [player];
      
      verify_player_in_game(game, 'player1');
      
      expect(verify_player_in_game).toHaveBeenCalledWith(game, 'player1');
    });
  });

  describe('verify_cards_in_players_hand', () => {
    it('should not throw when cards are in hand', () => {
      const player = createMockPlayer('player1', 'Player 1');
      const cards: Card[] = [{ suit: 0, value: 13 }, { suit: 1, value: 12 }];
      player.hand = cards;
      
      expect(() => verify_cards_in_players_hand(player, cards)).not.toThrow();
    });

    it('should throw when card is not in hand', () => {
      const player = createMockPlayer('player1', 'Player 1');
      player.hand = [{ suit: 0, value: 13 }];
      const cards: Card[] = [{ suit: 1, value: 12 }];
      
      expect(() => verify_cards_in_players_hand(player, cards)).toThrow('Card K of Hearts is not in player');
    });
  });

  describe('validate_defender_status', () => {
    it('should not throw when player is defender and should be', () => {
      const game = createMockGame();
      const player = createMockPlayer('player1', 'Player 1');
      game.players = [player];
      game.defender = 0;
      
      expect(() => validate_defender_status(game, 'player1', true)).not.toThrow();
    });

    it('should throw when player is not defender but should be', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      game.players = [player1, player2];
      game.defender = 1;
      
      expect(() => validate_defender_status(game, 'player1', true)).toThrow('Player player1 is not the defender');
    });
  });

  describe('no_cards_left', () => {
    it('should return true when deck and flipped are empty', () => {
      const game = createMockGame();
      game.deck = [];
      game.flipped = null;
      
      expect(no_cards_left(game)).toBe(true);
    });

    it('should return false when deck has cards', () => {
      const game = createMockGame();
      game.deck = [{ suit: 0, value: 13 }];
      game.flipped = null;
      
      expect(no_cards_left(game)).toBe(false);
    });

    it('should return false when flipped card exists', () => {
      const game = createMockGame();
      game.deck = [];
      game.flipped = { suit: 0, value: 13 };
      
      expect(no_cards_left(game)).toBe(false);
    });
  });

  describe('draw', () => {
    it('should draw card from deck', () => {
      const game = createMockGame();
      const testCard: Card = { suit: 0, value: 13 };
      game.deck = [testCard];
      
      const drawn = draw(game);
      expect(drawn).toEqual(testCard);
      expect(game.deck).toHaveLength(0);
    });

    it('should draw flipped card when deck is empty', () => {
      const game = createMockGame();
      const testCard: Card = { suit: 0, value: 13 };
      game.deck = [];
      game.flipped = testCard;
      
      const drawn = draw(game);
      expect(drawn).toEqual(testCard);
      expect(game.flipped).toBeNull();
    });

    it('should return null when no cards available', () => {
      const game = createMockGame();
      game.deck = [];
      game.flipped = null;
      
      const drawn = draw(game);
      expect(drawn).toBeNull();
    });
  });

  describe('refill_deck', () => {
    it('should create full deck for 2 players', () => {
      const deck = refill_deck(2);
      expect(deck).toHaveLength(36); // 9 values (5-13) * 4 suits
      expect(deck.every(card => card.value >= 5)).toBe(true);
    });

    it('should create full deck for 6 players', () => {
      const deck = refill_deck(6);
      expect(deck).toHaveLength(52); // 13 values (1-13) * 4 suits
      expect(deck.some(card => card.value === 1)).toBe(true);
    });

    it('should have all 4 suits', () => {
      const deck = refill_deck(4);
      const suits = new Set(deck.map(card => card.suit));
      expect(suits.size).toBe(4);
      expect(suits.has(0)).toBe(true); // Spades
      expect(suits.has(1)).toBe(true); // Hearts
      expect(suits.has(2)).toBe(true); // Clubs
      expect(suits.has(3)).toBe(true); // Diamonds
    });
  });

  describe('initialize_hands', () => {
    it('should deal 6 cards to each player', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      game.players = [player1, player2];
      game.deck = refill_deck(2);
      
      const hands = initialize_hands(game);
      expect(hands).toHaveLength(2);
      expect(hands[0]).toHaveLength(6);
      expect(hands[1]).toHaveLength(6);
      expect(game.deck).toHaveLength(24); // 36 - 12 dealt
    });

    it('should deal different cards to each player', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      game.players = [player1, player2];
      game.deck = refill_deck(2);
      
      const hands = initialize_hands(game);
      const allCards = [...hands[0], ...hands[1]];
      const uniqueCards = new Set(allCards.map(card => `${card.suit}-${card.value}`));
      expect(uniqueCards.size).toBe(12); // All cards should be unique
    });
  });

  describe('determine_lowest_power_index', () => {
    it('should find player with lowest power card', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.hand = [{ suit: 0, value: 13 }, { suit: 1, value: 12 }]; // A of Spades, K of Hearts
      player2.hand = [{ suit: 0, value: 5 }, { suit: 1, value: 6 }]; // 6 of Spades, 7 of Hearts
      
      game.players = [player1, player2];
      game.power_suit = 0; // Spades
      
      const result = determine_lowest_power_index(game);
      expect(result).toBe(1); // Player 2 has 6 of Spades
    });

    it('should return random player when no power cards', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.hand = [{ suit: 1, value: 13 }, { suit: 2, value: 12 }]; // Hearts, Clubs
      player2.hand = [{ suit: 1, value: 5 }, { suit: 2, value: 6 }]; // Hearts, Clubs
      
      game.players = [player1, player2];
      game.power_suit = 0; // Spades (no one has spades)
      
      const result = determine_lowest_power_index(game);
      expect(result).toBeGreaterThanOrEqual(0);
      expect(result).toBeLessThan(2);
    });
  });

  describe('set_positions', () => {
    it('should set defender as next player after first attacker', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      const player3 = createMockPlayer('player3', 'Player 3');
      
      game.players = [player1, player2, player3];
      game.first_attacker = 0;
      
      set_positions(game);
      expect(game.defender).toBe(1);
    });

    it('should wrap around for defender position', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      game.players = [player1, player2];
      game.first_attacker = 1;
      
      set_positions(game);
      expect(game.defender).toBe(0);
    });
  });

  describe('game_done', () => {
    it('should return null when game is not finished', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.status = PLAYER_STATUS.IN;
      player2.status = PLAYER_STATUS.IN;
      
      game.players = [player1, player2];
      
      const result = game_done(game);
      expect(result).toBeNull();
    });

    it('should return player id when only one player left', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.status = PLAYER_STATUS.IN;
      player2.status = PLAYER_STATUS.OUT;
      
      game.players = [player1, player2];
      
      const result = game_done(game);
      expect(result).toBe('player1');
    });

    it('should return null when no players are left', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.status = PLAYER_STATUS.OUT;
      player2.status = PLAYER_STATUS.OUT;
      
      game.players = [player1, player2];
      
      const result = game_done(game);
      expect(result).toBeNull();
    });
  });

  describe('personalize_game', () => {
    it('should be mocked (database-dependent function)', () => {
      const game = createMockGame();
      const player1 = createMockPlayer('player1', 'Player 1');
      const player2 = createMockPlayer('player2', 'Player 2');
      
      player1.hand = [{ suit: 0, value: 13 }];
      player2.hand = [{ suit: 1, value: 12 }];
      
      game.players = [player1, player2];
      
      personalize_game(game, 'player1');
      
      expect(personalize_game).toHaveBeenCalledWith(game, 'player1');
    });
  });
}); 