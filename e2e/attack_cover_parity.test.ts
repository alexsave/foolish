/* =============================================================================
 * Attack + cover legality — client vs kernel parity fuzz
 * =============================================================================
 * The pass_parity suite polices canPass; this file extends the same pattern
 * to the other two hand-rolled client validators:
 *
 *   SERVER — actions/attack.ts validateAttack, actions/cover.ts validateCover
 *            (both kernel-backed; throw == illegal)
 *   CLIENT — canAttack / validateAttack / canCoverCards / validateCover
 *            (src/utils/gameValidation.ts — the UI button / optimistic gates)
 *
 * Invariants asserted on random kernel-played game states:
 *   1. ATTACK: for candidate sets from the acting player's own hand (unique,
 *      non-defender — the preconditions every UI caller establishes), the
 *      client and the kernel must agree exactly. This includes the
 *      first-attacker restriction on an empty table (the old client showed a
 *      live Attack button to every non-defender).
 *   2. COVER (button): whenever canCoverCards says yes, the mapping the
 *      client would submit (findUnambiguousCover) must be kernel-legal.
 *      The reverse is deliberately NOT asserted — an ambiguous cover is
 *      hidden by design even though some mapping would be legal.
 *   3. COVER (optimistic gate): the throwing validateCover must agree with
 *      the kernel on defender-owned mappings, in both directions.
 *
 * Pure in-memory (no Postgres): games are driven by the real engine via
 * processBotAction, exactly like replay_codec.test.ts.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { game_done, personalize_game } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import {
  Card,
  Game,
  GAME_STATUS,
  PLAYER_STATUS,
  PersonalGame,
  PrivatePlayer,
  StrategyKey,
} from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { validateAttack as serverValidateAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { validateCover as serverValidateCover } from '../supabase/functions/_shared/actions/cover.ts';
import {
  canAttack as clientCanAttack,
  validateAttack as clientValidateAttack,
  canCoverCards as clientCanCoverCards,
  validateCover as clientValidateCover,
} from '../src/utils/gameValidation.ts';
import { kernelUnambiguousCover } from '../supabase/functions/_shared/wasm/bots.ts';

// The engine logs play-by-play; keep the reporter readable.
if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.error = () => {};
  console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.PARITY_GAMES_PER_PC ?? 8);
const MAX_ACTIONS = 100000;
const cardKey = (c: Card) => `${c.suit}:${c.value}`;

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
  player_id: `bot_${i}`,
  name: `Bot ${i}`,
  status: PLAYER_STATUS.READY,
  is_ai: true,
  hand: [],
  awaiting_attack: false,
  hand_length: 0,
  strategy_key: strategy,
});

const mkGame = (np: number, strategy: StrategyKey): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i, strategy)),
  deck: [],
  logs: [],
  id: 'g',
  name: 'g',
  status: GAME_STATUS.PLAYING,
  deck_length: 0,
  discard_pile_length: 0,
  flipped: null,
  power_suit: 0,
  first_attacker: 0,
  defender: 0,
  table_battles: [],
  elimination_order: [],
  good_timestamp: null,
  good_players: [],
});

function serverAllowsAttack(game: Game, pid: string, cards: Card[]): boolean {
  try { serverValidateAttack(game, pid, cards); return true; } catch { return false; }
}
function serverAllowsCover(game: Game, pid: string, covers: Card[], attacks: Card[]): boolean {
  try { serverValidateCover(game, pid, covers, attacks); return true; } catch { return false; }
}
function clientAllowsAttackOptimistic(personal: PersonalGame, cards: Card[]): boolean {
  try { clientValidateAttack(personal, cards); return true; } catch { return false; }
}
function clientAllowsCoverOptimistic(personal: PersonalGame, covers: Card[], attacks: Card[]): boolean {
  try { clientValidateCover(personal, covers, attacks); return true; } catch { return false; }
}

// Candidate attack sets from a hand: every same-value subset (the shapes the
// UI can actually submit), capped to keep the state count sane.
function candidateAttackSets(hand: Card[]): Card[][] {
  const byValue = new Map<number, Card[]>();
  for (const c of hand) (byValue.get(c.value) ?? byValue.set(c.value, []).get(c.value)!).push(c);
  const out: Card[][] = [];
  for (const cards of byValue.values()) {
    const n = Math.min(cards.length, 4);
    for (let mask = 1; mask < (1 << n); mask++) {
      const set: Card[] = [];
      for (let i = 0; i < n; i++) if (mask & (1 << i)) set.push(cards[i]);
      out.push(set);
    }
  }
  // one mixed-value negative per hand (must be rejected on an empty table)
  if (hand.length >= 2 && hand[0].value !== hand[1].value) out.push([hand[0], hand[1]]);
  return out;
}

// Random-ish but deterministic per (state, i) mapping candidates for the
// optimistic-cover gate: pair each of up to 2 hand cards with each uncovered
// attack (legal and illegal pairs both matter — the gates must AGREE).
function candidateCoverMappings(hand: Card[], game: Game): { covers: Card[]; attacks: Card[] }[] {
  const uncovered = game.table_battles.filter((b) => !b.defense).map((b) => b.attack);
  const out: { covers: Card[]; attacks: Card[] }[] = [];
  for (const h of hand.slice(0, 2)) {
    for (const a of uncovered) out.push({ covers: [h], attacks: [a] });
  }
  // a two-card mapping incl. a deliberately duplicated target
  if (hand.length >= 2 && uncovered.length >= 2) {
    out.push({ covers: [hand[0], hand[1]], attacks: [uncovered[0], uncovered[1]] });
    out.push({ covers: [hand[0], hand[1]], attacks: [uncovered[0], uncovered[0]] });
  }
  return out;
}

async function playAndCheck(np: number, strategy: StrategyKey, stats: { states: number; attacks: number; covers: number }): Promise<boolean> {
  const game = mkGame(np, strategy);
  start_game(game);
  let actions = 0;
  while (game_done(game) === null) {
    if (++actions > MAX_ACTIONS) return false;

    // ---- parity checks on the CURRENT state --------------------------------
    stats.states++;
    for (let seat = 0; seat < game.players.length; seat++) {
      const p = game.players[seat];
      if (p.status !== PLAYER_STATUS.IN) continue;
      const personal = personalize_game(game, p.player_id) as PersonalGame;

      if (seat !== game.defender) {
        // 1. ATTACK: exact agreement (candidates satisfy the callers'
        //    preconditions: own hand, unique, non-defender)
        for (const cards of candidateAttackSets(p.hand)) {
          const server = serverAllowsAttack(game, p.player_id, cards);
          const button = clientCanAttack(personal, cards);
          const optimistic = clientAllowsAttackOptimistic(personal, cards);
          stats.attacks++;
          const detail = `seat=${seat} first_attacker=${game.first_attacker} defender=${game.defender} `
            + `table=[${game.table_battles.map((b) => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}] `
            + `cards=[${cards.map(cardKey).join(',')}]`;
          assert.equal(button, server, `canAttack !== kernel: ${detail}`);
          assert.equal(optimistic, server, `validateAttack !== kernel: ${detail}`);
        }
      } else {
        // 2. COVER button: offered => kernel-legal
        const uncovered = game.table_battles.filter((b) => !b.defense);
        if (uncovered.length > 0 && p.hand.length > 0) {
          const selections: Card[][] = p.hand.map((c) => [c]);
          if (p.hand.length >= 2) selections.push([p.hand[0], p.hand[1]]);
          for (const sel of selections) {
            if (!clientCanCoverCards(personal, sel)) continue;
            const mapping = kernelUnambiguousCover(sel, game.table_battles, game.power_suit);
            assert.ok(mapping, 'canCoverCards true but no unambiguous mapping');
            stats.covers++;
            assert.ok(
              serverAllowsCover(game, p.player_id, mapping!.coverCards, mapping!.attackCards),
              `client offers a cover the kernel rejects: covers=[${mapping!.coverCards.map(cardKey).join(',')}] `
              + `attacks=[${mapping!.attackCards.map(cardKey).join(',')}]`,
            );
          }
          // 3. COVER optimistic gate: exact agreement on explicit mappings
          for (const m of candidateCoverMappings(p.hand, game)) {
            const server = serverAllowsCover(game, p.player_id, m.covers, m.attacks);
            const optimistic = clientAllowsCoverOptimistic(personal, m.covers, m.attacks);
            stats.covers++;
            assert.equal(
              optimistic, server,
              `validateCover !== kernel: covers=[${m.covers.map(cardKey).join(',')}] `
              + `attacks=[${m.attacks.map(cardKey).join(',')}] `
              + `table=[${game.table_battles.map((b) => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}]`,
            );
          }
        }
      }
    }

    // ---- advance the game with the real engine -----------------------------
    const eligible: PrivatePlayer[] = [];
    for (let i = 0; i < game.players.length; i++) {
      const p = game.players[i];
      if (shouldBotActCore(game, p, i) && calculateLegalMoves(game, p.player_id).length > 0) {
        eligible.push(p);
      }
    }
    if (eligible.length === 0) return false;
    const order = [...eligible];
    for (let i = order.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [order[i], order[j]] = [order[j], order[i]];
    }
    let acted = false;
    for (const p of order) {
      if (await processBotAction(game, p)) { acted = true; break; }
    }
    if (!acted) return false;
  }
  return true;
}

if (!process.env.VALIDATION_ONLY) {
  test(`attack/cover parity fuzz: client gates agree with the kernel (${GAMES_PER_PC}/player-count, 2..5 players)`, async () => {
    const stats = { states: 0, attacks: 0, covers: 0 };
    let played = 0;
    for (let np = 2; np <= 5; np++) {
      for (let g = 0; g < GAMES_PER_PC; g++) {
        const strategy = (g % 2 === 0 ? 'random' : 'handwritten') as StrategyKey;
        if (await playAndCheck(np, strategy, stats)) played++;
      }
    }
    // eslint-disable-next-line no-console
    console.error(`[attack-cover-parity] games=${played} states=${stats.states} attackChecks=${stats.attacks} coverChecks=${stats.covers}`);
    assert.ok(played > 0, 'no games completed');
    assert.ok(stats.attacks > 0 && stats.covers > 0, 'fuzz exercised both dimensions');
  });
}
