// Deterministic repro for the Q4 finding (no concurrency, no DB): validateCover
// checks each attack card is on the table by VALUE only
//   (cover.ts:32  battle.attack.value === card.value && battle.defense === null)
// but executeCover locates it by EXACT suit+value
//   (cover.ts:72  card_comp(battle.attack, attack_card) && battle.defense === null).
//
// So when two same-value attacks are on the table and the specific one the
// defender names is already covered, validation passes (the OTHER same-value
// attack satisfies the value check) and execution throws the uncaught
// 'SEVERE: Card not found on table'. In production this is reachable by a
// defender double-tapping "cover" on one of two same-rank attacks (the second
// tap reloads a state where that exact card is already covered).

import { Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../../supabase/functions/_shared/types.ts';
import { handleCover } from '../../supabase/functions/_shared/actions/cover.ts';

const card = (suit: number, value: number) => ({ suit, value });

function makeGame(): Game {
  return {
    id: 'repro', name: 'repro', deck_length: 0, discard_pile_length: 0,
    flipped: null, status: GAME_STATUS.PLAYING, power_suit: 0, // spades trump
    first_attacker: 0, defender: 1, table_battles: [
      { attack: card(0, 6), defense: null }, // 7 of spades  (value 6)
      { attack: card(1, 6), defense: null }, // 7 of hearts  (value 6)
    ],
    elimination_order: [], good_timestamp: null, good_players: [],
    deck: [], logs: [],
    players: [
      { player_id: 'attacker', name: 'Attacker', status: PLAYER_STATUS.IN, is_ai: false, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.HUMAN },
      // defender holds two spade trumps that can each cover a 7
      { player_id: 'defender', name: 'Defender', status: PLAYER_STATUS.IN, is_ai: false, hand: [card(0, 7), card(0, 8)], awaiting_attack: false, hand_length: 2, strategy_key: STRATEGY_KEY.HUMAN },
    ],
  };
}

console.log('=== Q4 deterministic repro: cover validate/execute matching mismatch ===');
const game = makeGame();

// First cover: 7♠ (value 6, suit 0) with 8♠ — legitimate.
handleCover(game, 'defender', [card(0, 7)], [card(0, 6)]);
console.log('after first cover, table:', game.table_battles.map(b => `${b.attack.suit}:${b.attack.value}${b.defense ? `/def ${b.defense.suit}:${b.defense.value}` : ''}`).join('  '));

// Second cover (the realistic stale double-tap): defender again names 7♠, which
// is now covered. 7♥ (same value) is still uncovered, so validateCover passes.
try {
  handleCover(game, 'defender', [card(0, 8)], [card(0, 6)]);
  console.log('Q4 repro: NO error (unexpected)');
} catch (e: any) {
  const severe = String(e.message).includes('SEVERE');
  console.log(`Q4 repro: threw -> "${e.message}"`);
  console.log(severe
    ? 'CONFIRMED BUG: validation passed (value match on the other 7) but execution could not find the exact covered card -> uncaught SEVERE (surfaces as a 500-class error to the user).'
    : 'threw a different (graceful) error');
  process.exit(severe ? 0 : 1);
}
