import { validatePickup, executePickup, handlePickup } from '../../supabase/functions/_shared/actions/pickup';
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
  validate_defender_status 
} from '../../supabase/functions/_shared/common_utils';

// Mock only the complex utils that require database operations
jest.mock('../../supabase/functions/_shared/utils', () => ({
  refill: jest.fn(),
}));

describe('Pickup Action Tests', () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  describe('validatePickup', () => {
    it('should throw error when game is not in FREE_PLAY or ONLY_DEFEND', () => {
      const game = createTwoPlayerGame(GAME_STATUS.FIRST_ATTACKER);

      expectError(
        () => validatePickup(game, 'defender-id'),
        'Game test-game-id is not in free_play or only_defend mode'
      );
    });

    it('should pass validation for FREE_PLAY status', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      expect(() => validatePickup(game, 'defender-id')).not.toThrow();
    });

    it('should pass validation for ONLY_DEFEND status', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.ONLY_DEFEND, attackerHand, defenderHand, table_battles);

      expect(() => validatePickup(game, 'defender-id')).not.toThrow();
    });

    it('should throw error when player is not defender', () => {
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, [], [], table_battles);
      
      // Try to pickup as the attacker (player 0) instead of defender (player 1)
      expectError(
        () => validatePickup(game, 'attacker-id'),
        'Player attacker-id is not the defender'
      );
    });

    it('should throw error when no cards on table', () => {
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY); // No table battles

      expectError(
        () => validatePickup(game, 'defender-id'),
        'No cards on the table'
      );
    });

    it('should pass validation when there are cards on table', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      expect(() => validatePickup(game, 'defender-id')).not.toThrow();
    });
  });

  describe('executePickup', () => {
    it('should pickup uncovered attack cards', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that attack card was added to defender's hand
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
      
      // Check that table was cleared
      expect(game.table_battles).toEqual([]);
      
      // Check that positions were updated
      // first_attacker = get_next_player_index(game, defender) = get_next_player_index(game, 1) = 0
      // defender = get_next_player_index(game, first_attacker) = get_next_player_index(game, 0) = 1
      expect(game.first_attacker).toBe(0);
      expect(game.defender).toBe(1);
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
      
      // Check that done_attacking_this_round flags were reset
      expect(game.players[0].done_attacking_this_round).toBe(false);
      expect(game.players[1].done_attacking_this_round).toBe(false);
    });

    it('should pickup both attack and defense cards when covered', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.EIGHT_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that both attack and defense cards were added to defender's hand
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.EIGHT_HEARTS);
      
      // Check that table was cleared
      expect(game.table_battles).toEqual([]);
    });

    it('should pickup multiple battles with mixed coverage', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [
        createMockBattle(COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.EIGHT_HEARTS), // Covered
        createMockBattle(COMMON_CARDS.NINE_HEARTS), // Uncovered
        createMockBattle(COMMON_CARDS.TEN_HEARTS, COMMON_CARDS.JACK_HEARTS) // Covered
      ];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that all cards were added to defender's hand
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.EIGHT_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.NINE_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.TEN_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.JACK_HEARTS);
      
      // Check that table was cleared
      expect(game.table_battles).toEqual([]);
    });

    it('should handle positions correctly in three-player game', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS], // Player 1
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.NINE_HEARTS]   // Player 3
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.FREE_PLAY, hands, table_battles);

      executePickup(game, 'player2-id');

      // Check that first_attacker is set correctly
      expect(game.first_attacker).toBe(2);
      expect(game.defender).toBe(0);
    });

    it('should handle empty table gracefully', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles: any[] = [];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that defender's hand is unchanged
      expect(game.players[1].hand).toEqual([COMMON_CARDS.NINE_HEARTS]);
      
      // Check that table remains empty
      expect(game.table_battles).toEqual([]);
      
      // Check that game state was updated
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });

    it('should preserve original defender hand contents', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS, COMMON_CARDS.TEN_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that original hand contents are preserved
      expect(game.players[1].hand).toContain(COMMON_CARDS.NINE_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.TEN_HEARTS);
      
      // Check that picked up card is added
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
    });

    it('should handle battles with null defense correctly', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [
        createMockBattle(COMMON_CARDS.SEVEN_HEARTS, null), // Explicitly null defense
        createMockBattle(COMMON_CARDS.EIGHT_HEARTS) // Implicitly null defense
      ];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      executePickup(game, 'defender-id');

      // Check that only attack cards were added
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
      expect(game.players[1].hand).toContain(COMMON_CARDS.EIGHT_HEARTS);
      
      // Check that no null values were added
      expect(game.players[1].hand).not.toContain(null);
      expect(game.players[1].hand).not.toContain(undefined);
    });
  });

  describe('handlePickup', () => {
    it('should call both validate and execute functions', () => {
      const attackerHand = [COMMON_CARDS.SEVEN_HEARTS];
      const defenderHand = [COMMON_CARDS.NINE_HEARTS];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY, attackerHand, defenderHand, table_battles);

      handlePickup(game, 'defender-id');

      // Verify that execution occurred (card was picked up)
      expect(game.players[1].hand).toContain(COMMON_CARDS.SEVEN_HEARTS);
      expect(game.table_battles).toEqual([]);
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });

    it('should throw validation errors before execution', () => {
      const game = createTwoPlayerGame(GAME_STATUS.FIRST_ATTACKER);

      expectError(
        () => handlePickup(game, 'defender-id'),
        'Game test-game-id is not in free_play or only_defend mode'
      );
      
      // Verify that execution did not occur
      expect(game.table_battles).toHaveLength(0);
    });
  });
}); 