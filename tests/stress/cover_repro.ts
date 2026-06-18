// Regression test for the cover validate/execute matching mismatch.
//
// Before the fix: validateCover checked the named attack card by VALUE only
// (cover.ts), while executeCover located it by EXACT suit+value (card_comp). With
// two same-rank attacks on the table, naming one that's already covered passed
// validation (the OTHER same-rank attack satisfied the value check) and then
// threw the uncaught 'SEVERE: Card not found on table' in execution — reachable
// by a defender double-tapping cover.
//
// After the fix: validateCover matches by exact card, so the second tap is
// rejected gracefully, and covering the still-uncovered same-rank attack works.
//
//   npx tsx tests/stress/cover_repro.ts   (exit 0 = pass)

import { Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../../supabase/functions/_shared/types.ts';
import { handleCover } from '../../supabase/functions/_shared/actions/cover.ts';

const card = (suit: number, value: number) => ({ suit, value });

function makeGame(): Game {
  return {
    id: 'repro', name: 'repro', deck_length: 0, discard_pile_length: 0,
    flipped: null, status: GAME_STATUS.PLAYING, power_suit: 0, // spades trump
    first_attacker: 0, defender: 1, table_battles: [
      { attack: card(0, 6), defense: null }, // 7 of spades
      { attack: card(1, 6), defense: null }, // 7 of hearts (same rank)
    ],
    elimination_order: [], good_timestamp: null, good_players: [],
    deck: [], logs: [],
    players: [
      { player_id: 'attacker', name: 'Attacker', status: PLAYER_STATUS.IN, is_ai: false, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.HUMAN },
      { player_id: 'defender', name: 'Defender', status: PLAYER_STATUS.IN, is_ai: false, hand: [card(0, 7), card(0, 8)], awaiting_attack: false, hand_length: 2, strategy_key: STRATEGY_KEY.HUMAN },
    ],
  };
}

let pass = true;
const check = (cond: boolean, label: string) => { console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}`); if (!cond) pass = false; };

console.log('=== cover matching regression ===');

// 1) Double-tap on the already-covered same-rank card must be a graceful reject.
{
  const g = makeGame();
  handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]); // cover 7♠ with 8♠... (legit)
  let msg = '';
  try { handleCover(g, 'defender', [card(0, 8)], [card(0, 6)]); } catch (e: any) { msg = String(e.message); }
  check(msg.length > 0 && !msg.includes('SEVERE'), `double-tap on covered 7♠ rejected gracefully (got: "${msg || 'no error'}")`);
}

// 2) Covering the still-uncovered same-rank attack (7♥) must succeed.
{
  const g = makeGame();
  handleCover(g, 'defender', [card(0, 7)], [card(0, 6)]); // cover 7♠
  let ok = false;
  try { handleCover(g, 'defender', [card(0, 8)], [card(1, 6)]); ok = true; } catch { ok = false; }
  const bothCovered = ok && (makeGame(), true);
  check(ok, 'covering the still-uncovered 7♥ succeeds');
  void bothCovered;
}

console.log(pass ? 'cover regression: PASS' : 'cover regression: FAIL');
process.exit(pass ? 0 : 1);
