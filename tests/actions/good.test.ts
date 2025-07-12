import { validateGood, executeGood, handleGood } from '../../supabase/functions/_shared/actions/good';
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
import { get_next_player_index } from '../../supabase/functions/_shared/common_utils';

describe('Good Action Tests', () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  describe('validateGood', () => {
    it('should throw error when game is not in WAIT_FOR_ATTACKERS', () => {
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY);

      expectError(
        () => validateGood(game, 'player-id'),
        'Game test-game-id is not in wait_for_attackers mode'
      );
    });

    it('should throw error when player is not IN status', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].status = PLAYER_STATUS.OUT;

      expectError(
        () => validateGood(game, 'attacker-id'),
        'Player attacker-id is not ready to attack'
      );
    });

    it('should pass validation for valid good move', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].status = PLAYER_STATUS.IN;

      expect(() => validateGood(game, 'attacker-id')).not.toThrow();
    });

    it('should pass validation for IN status', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].status = PLAYER_STATUS.IN;

      expect(() => validateGood(game, 'attacker-id')).not.toThrow();
    });

    it('should throw error for IDLE status', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].status = PLAYER_STATUS.IDLE;

      expectError(
        () => validateGood(game, 'attacker-id'),
        'Player attacker-id is not ready to attack'
      );
    });
  });

  describe('executeGood', () => {
    it('should clear awaiting_attack flag for player', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].awaiting_attack = true;

      executeGood(game, 'attacker-id');

      expect(game.players[0].awaiting_attack).toBe(false);
    });

    it('should not end round when other players are still awaiting attack', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS], // Player 1
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.SEVEN_CLUBS]   // Player 3 - has matching value
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = true;

      executeGood(game, 'player1-id');

      // Check that player 1's flag was cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Check that player 3 is still awaiting
      expect(game.players[2].awaiting_attack).toBe(true);
      
      // Check that game state was not changed
      expect(game.status).toBe(GAME_STATUS.WAIT_FOR_ATTACKERS);
      expect(game.table_battles).toHaveLength(1);
    });

    it('should end round when no players are awaiting attack', () => {
      const hands = [
        [COMMON_CARDS.EIGHT_HEARTS], // attacker - has 8 value
        [COMMON_CARDS.NINE_HEARTS], // defender
        [COMMON_CARDS.TEN_HEARTS]   // other player - no matching cards
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.EIGHT_CLUBS)]; // 7 attacked, 8 defended
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // No players have awaiting_attack set since no one has matching cards
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = false; // No matching cards

      executeGood(game, 'player1-id');

      // Check that awaiting_attack was cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Check that table was cleared
      expect(game.table_battles).toEqual([]);
      
      // Check that positions were updated
      // first_attacker = game.defender (1)
      // defender = get_next_player_index(game, 1) = 2
      expect(game.first_attacker).toBe(1); // should be the old defender index
      expect(game.defender).toBe(2); // Next player after defender
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
      
      // Check that done_attacking_this_round flags were reset
      expect(game.players[0].done_attacking_this_round).toBe(false);
      expect(game.players[1].done_attacking_this_round).toBe(false);
      expect(game.players[2].done_attacking_this_round).toBe(false);
    });

    it('should handle defender correctly when checking playable players', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS], // Player 1 (attacker)
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.SEVEN_CLUBS]   // Player 3 - has matching value
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_HEARTS)];
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set defender index
      game.defender = 1;
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = true;

      executeGood(game, 'player1-id');

      // Defender should not be considered in playable players check
      expect(game.players[1].awaiting_attack).toBe(false);
      
      // Only player 3 should still be awaiting
      expect(game.players[2].awaiting_attack).toBe(true);
      
      // Game should not end yet
      expect(game.status).toBe(GAME_STATUS.WAIT_FOR_ATTACKERS);
    });

    it('should check for matching attack card values', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS], // Player 1 - has 7 (matches attack)
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.NINE_CLUBS]    // Player 3 - has 9 (no match)
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_DIAMONDS)]; // 7 attack
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = false;

      executeGood(game, 'player1-id');

      // Player 1 should have been cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Player 3 doesn't have matching cards, so game should end
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });

    it('should check for matching defense card values', () => {
      const hands = [
        [COMMON_CARDS.NINE_HEARTS], // Player 1 - has 9 (matches defense)
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.TEN_CLUBS]     // Player 3 - has 10 (no match)
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_DIAMONDS, COMMON_CARDS.NINE_CLUBS)]; // 7 attack, 9 defense
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = false;

      executeGood(game, 'player1-id');

      // Player 1 should have been cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Player 3 doesn't have matching cards, so game should end
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });

    it('should handle multiple battles with mixed values', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS, COMMON_CARDS.NINE_HEARTS], // Player 1 - has 7 and 9
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.TEN_CLUBS]     // Player 3 - has 10 (no match)
      ];
      const table_battles = [
        createMockBattle(COMMON_CARDS.SEVEN_DIAMONDS, COMMON_CARDS.NINE_CLUBS), // 7 attack, 9 defense
                 createMockBattle(COMMON_CARDS.TEN_HEARTS) // 10 attack, no defense
      ];
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = false;

      executeGood(game, 'player1-id');

      // Player 1 should have been cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Player 3 doesn't have matching cards, so game should end
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });

    it('should handle battles with null defense correctly', () => {
      const hands = [
        [COMMON_CARDS.SEVEN_HEARTS], // Player 1 - has 7
        [COMMON_CARDS.EIGHT_HEARTS], // Player 2 (defender)
        [COMMON_CARDS.NINE_CLUBS]    // Player 3 - has 9 (no match)
      ];
      const table_battles = [createMockBattle(COMMON_CARDS.SEVEN_DIAMONDS, null)]; // 7 attack, null defense
      const game = createThreePlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS, hands, table_battles);
      
      // Set up awaiting_attack flags
      game.players[0].awaiting_attack = true;
      game.players[2].awaiting_attack = false;

      executeGood(game, 'player1-id');

      // Player 1 should have been cleared
      expect(game.players[0].awaiting_attack).toBe(false);
      
      // Player 3 doesn't have matching cards, so game should end
      expect(game.status).toBe(GAME_STATUS.FIRST_ATTACKER);
    });
  });

  describe('handleGood', () => {
    it('should call both validate and execute functions', () => {
      const game = createTwoPlayerGame(GAME_STATUS.WAIT_FOR_ATTACKERS);
      game.players[0].status = PLAYER_STATUS.IN;
      game.players[0].awaiting_attack = true;

      handleGood(game, 'attacker-id');

      // Verify that execution occurred (awaiting_attack flag cleared)
      expect(game.players[0].awaiting_attack).toBe(false);
    });

    it('should throw validation errors before execution', () => {
      const game = createTwoPlayerGame(GAME_STATUS.FREE_PLAY);

      expectError(
        () => handleGood(game, 'attacker-id'),
        'Game test-game-id is not in wait_for_attackers mode'
      );
      
      // Verify that execution did not occur
      expect(game.players[0].awaiting_attack).toBe(false);
    });
  });
}); 