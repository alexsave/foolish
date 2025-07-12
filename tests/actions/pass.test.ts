import { validatePass, executePass, handlePass } from '../../supabase/functions/_shared/actions/pass';
import { GAME_STATUS, PLAYER_STATUS, Card, Game } from '../../supabase/functions/_shared/types';
import { 
  createTwoPlayerGame, 
  createThreePlayerGame, 
  createMockBattle, 
  COMMON_CARDS,
  expectError,
  expectAsyncError,
  createMockPlayer 
} from '../helpers/gameHelpers';

// Import the real implementations we want to test
import { 
  get_next_player_index, 
  card_comp, 
  cardDisplay, 
  validate_defender_status, 
  verify_cards_in_players_hand, 
  no_cards_left 
} from '../../supabase/functions/_shared/common_utils';

// Mock only the complex utils that require database operations
jest.mock('../../supabase/functions/_shared/utils', () => ({
  check_win: jest.fn(),
}));

describe('Pass Action Tests', () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  describe('validatePass', () => {
    it('should throw error when no cards provided', () => {
      const game = createTwoPlayerGame();
      expectError(() => validatePass(game, 'defender-id', null as any), 'No cards provided');
      expectError(() => validatePass(game, 'defender-id', undefined as any), 'No cards provided');
    });

    it('should throw error when cards have different values', () => {
      const game = createTwoPlayerGame();
      const mixedCards = [COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.EIGHT_HEARTS];
      
      expectError(
        () => validatePass(game, 'defender-id', mixedCards),
        'Cards 7 of Hearts, 8 of Hearts are not all the same value'
      );
    });

    it('should throw error when cards have duplicates', () => {
      const game = createTwoPlayerGame();
      const duplicateCards = [COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.SEVEN_HEARTS];
      
      expectError(
        () => validatePass(game, 'defender-id', duplicateCards),
        'Cards 7 of Hearts, 7 of Hearts have duplicates'
      );
    });

    it('should throw error when player is not defender', () => {
      const game = createTwoPlayerGame();
      const cards = [COMMON_CARDS.SEVEN_HEARTS];
      
      // Try to pass as the attacker (player 0) instead of defender (player 1)
      expectError(
        () => validatePass(game, 'attacker-id', cards),
        'Player attacker-id is not the defender'
      );
    });

    it('should throw error when card is not in defenders hand', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.EIGHT_HEARTS]; // Defender doesn't have SEVEN_HEARTS
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_HEARTS]; // Card not in hand

      expectError(
        () => validatePass(game, 'defender-id', cards),
        'Card 7 of Hearts is not in player defender-id\'s hand'
      );
    });

    it('should throw error when no cards on table', () => {
      const game = createTwoPlayerGame(); // No table battles
      const cards = [COMMON_CARDS.SEVEN_HEARTS];

      expectError(
        () => validatePass(game, 'defender-id', cards),
        'No cards on the table'
      );
    });

    it('should throw error when cover is present', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.EIGHT_HEARTS)]; // Has cover
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS];

      expectError(
        () => validatePass(game, 'defender-id', cards),
        'Cover present, cannot pass'
      );
    });

    it('should throw error when card values do not match table values', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.EIGHT_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)]; // Only 7 on table
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);
      
      const cards = [COMMON_CARDS.EIGHT_HEARTS]; // 8 does not match 7

      expectError(
        () => validatePass(game, 'defender-id', cards),
        'Cards 8 of Hearts do not match the values on the table'
      );
    });

    it('should throw error when next player has insufficient cards', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS]; // Only 1 card
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)]; // 1 card on table
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS]; // 1 card to pass + 1 on table = 2 total
      
      expectError(
        () => validatePass(game, 'player2-id', cards),
        'Player Player 3 does not have enough cards in their hand to cover 7 of Clubs'
      );
    });

    it('should pass validation for valid pass move', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS]; // 2 cards
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)]; // 1 card on table
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS]; // 1 card to pass + 1 on table = 2 total
      
      expect(() => validatePass(game, 'player2-id', cards)).not.toThrow();
    });

    it('should pass validation with multiple cards of same value', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS, COMMON_CARDS.TEN_HEARTS]; // 3 cards
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)]; // 1 card on table
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS]; // 2 cards to pass + 1 on table = 3 total
      
      expect(() => validatePass(game, 'player2-id', cards)).not.toThrow();
    });
  });

  describe('executePass', () => {
    it('should execute pass successfully', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS];
      
      await executePass(game, 'player2-id', cards);

      // Check that card was added to table
      expect(game.table_battles).toHaveLength(2);
      expect(game.table_battles[1]).toEqual({ attack: COMMON_CARDS.SEVEN_CLUBS, defense: null });
      
      // Check that card was removed from hand
      expect(game.players[1].hand).toEqual([]);
      
      // Check that defender was changed
      expect(game.defender).toBe(2);
    });

    it('should execute pass with multiple cards', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS, COMMON_CARDS.TEN_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS];
      
      await executePass(game, 'player2-id', cards);

      // Check that both cards were added to table
      expect(game.table_battles).toHaveLength(3);
      expect(game.table_battles[1]).toEqual({ attack: COMMON_CARDS.SEVEN_CLUBS, defense: null });
      expect(game.table_battles[2]).toEqual({ attack: COMMON_CARDS.SEVEN_DIAMONDS, defense: null });
      
      // Check that both cards were removed from hand
      expect(game.players[1].hand).toEqual([]);
    });

    it('should handle player elimination when no cards left and hand is empty', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS]; // Only 1 card
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      // Set up empty deck to simulate no cards available
      game.deck = [];
      game.flipped = null;
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS];

      await executePass(game, 'player2-id', cards);

      // Check that player was eliminated
      expect(game.players[1].status).toBe(PLAYER_STATUS.OUT);
      expect(game.elimination_order).toContain('player2-id');
      
      // Check that defender was changed
      expect(game.defender).toBe(2);
    });

    it('should not eliminate player when deck has cards', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      // Set up deck with cards
      game.deck = [COMMON_CARDS.TEN_HEARTS];
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS];

      await executePass(game, 'player2-id', cards);

      // Check that player was not eliminated
      expect(game.players[1].status).toBe(PLAYER_STATUS.IN);
      expect(game.elimination_order).not.toContain('player2-id');
    });

    it('should not eliminate player when hand is not empty', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.EIGHT_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      // Set up empty deck
      game.deck = [];
      game.flipped = null;
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS]; // Still has EIGHT_CLUBS

      await executePass(game, 'player2-id', cards);

      // Check that player was not eliminated (still has cards in hand)
      expect(game.players[1].status).toBe(PLAYER_STATUS.IN);
      expect(game.elimination_order).not.toContain('player2-id');
    });

    it('should set status to ONLY_DEFEND when uncovered cards equal defender cards', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS]; // 2 cards so can cover 2 attacks
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS]; // Will create 2 uncovered battles total
      
      await executePass(game, 'player2-id', cards);

      // Check that status changed to ONLY_DEFEND
      expect(game.status).toBe(GAME_STATUS.ONLY_DEFEND);
    });

    it('should throw error when uncovered cards exceed defender cards', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS]; // Only 1 card
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS, COMMON_CARDS.SEVEN_DIAMONDS]; // Will create 3 uncovered battles total
      
      await expectAsyncError(
        () => executePass(game, 'player2-id', cards),
        'Uncovered cards > defender_cards'
      );
    });

    it('should set status to FREE_PLAY when uncovered cards less than defender cards', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS, COMMON_CARDS.TEN_HEARTS]; // 3 cards
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS]; // Will create 2 uncovered battles total
      
      await executePass(game, 'player2-id', cards);

      // Check that status remained FREE_PLAY
      expect(game.status).toBe(GAME_STATUS.FREE_PLAY);
    });
  });

  describe('handlePass', () => {
    it('should call both validate and execute functions', async () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.SEVEN_CLUBS];
      const nextPlayerHand = [COMMON_CARDS.EIGHT_HEARTS, COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, [attackerHand, defenderHand, nextPlayerHand], table_battles);
      
      const cards = [COMMON_CARDS.SEVEN_CLUBS];
      
      await handlePass(game, 'player2-id', cards);

      // Verify that execution occurred (card was added to table)
      expect(game.table_battles).toHaveLength(2);
      expect(game.table_battles[1]).toEqual({ attack: COMMON_CARDS.SEVEN_CLUBS, defense: null });
      expect(game.players[1].hand).toEqual([]);
      expect(game.defender).toBe(2);
    });

    it('should throw validation errors before execution', async () => {
      const game = createTwoPlayerGame();
      
      await expectAsyncError(
        () => handlePass(game, 'defender-id', null as any),
        'No cards provided'
      );
      
      // Verify that execution did not occur
      expect(game.table_battles).toHaveLength(0);
    });
  });
}); 