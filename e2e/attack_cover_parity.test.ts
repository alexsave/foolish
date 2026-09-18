/* =============================================================================
 * Attack + cover legality - client vs kernel parity fuzz
 * =============================================================================
 * The pass_parity suite polices canPass; this file extends the same pattern
 * to the other two client validators:
 *
 *   SERVER - table_act on the row (the move path's own operation; applied ==
 *            legal)
 *   CLIENT - canAttack / validateAttack / canCoverCards / validateCover
 *            (src/utils/gameValidation.ts - the UI button / optimistic gates),
 *            on the board the seat's envelope reads to
 *
 * Invariants asserted on random kernel-played game states:
 *   1. ATTACK: for candidate sets from the acting player's own hand (unique,
 *      non-defender - the preconditions every UI caller establishes), the
 *      client and the kernel must agree exactly. This includes the
 *      first-attacker restriction on an empty table (the old client showed a
 *      live Attack button to every non-defender).
 *   2. COVER (button): whenever canCoverCards says yes, the mapping the
 *      client would submit (findUnambiguousCover) must be kernel-legal.
 *      The reverse is deliberately NOT asserted - an ambiguous cover is
 *      hidden by design even though some mapping would be legal.
 *   3. COVER (optimistic gate): the throwing validateCover must agree with
 *      the kernel on defender-owned mappings, in both directions.
 *
 * Pure in-memory (no Postgres). Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved
 * it off the TypeScript Game: the SERVER is the C Table's table_act on the row
 * (e2e/helpers/table_mem.ts), the CLIENT gates read the seat's envelope as the
 * web does, and games are dealt from a seed and advanced by the kernel's bot
 * cycle, one action at a time.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import type { TableView, ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import {
  canAttack as clientCanAttack,
  validateAttack as clientValidateAttack,
  coverGesture as clientCoverGesture,
  validateCover as clientValidateCover,
} from '../src/utils/gameValidation.ts';
import { MemTable, type MemBoard } from './helpers/table_mem.ts';
import { suiteRng } from './helpers/rng.ts';

// Seeded end to end: the deals come from this stream, and the bots' decisions
// from the deal seed, so a red run replays exactly. To widen the search, sweep
// the seed.
const rng = suiteRng('attack_cover_parity');
const dealSeed = (): Uint8Array => Uint8Array.from({ length: 32 }, () => rng.int(256));

if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.error = () => {};
  console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.PARITY_GAMES_PER_PC ?? 8);
const MAX_ACTIONS = 3000;
const cardKey = (c: Card) => `${c.suit}:${c.value}`;
const tableText = (b: MemBoard) => b.battles.map((x) => `${cardKey(x.attack)}${x.defense ? '/' + cardKey(x.defense) : ''}`).join(' ');

function serverAllows(t: MemTable, seat: number, wire: Uint8Array): boolean {
  const { rc } = t.probe(seat, wire);
  assert.ok(rc === L.TABLE_APPLIED || rc === L.TABLE_REJECTED, `a move is applied or rejected, got ${rc}`);
  return rc === L.TABLE_APPLIED;
}
const serverAllowsAttack = (t: MemTable, seat: number, cards: Card[]) => serverAllows(t, seat, encodeAction({ kind: 'attack', cards }));
const serverAllowsCover = (t: MemTable, seat: number, covers: Card[], attacks: Card[]) =>
  serverAllows(t, seat, encodeAction({ kind: 'cover', cards: covers, attack_cards: attacks }));
function clientAllowsAttackOptimistic(personal: TableView, cards: Card[]): boolean {
  try { clientValidateAttack(personal, cards); return true; } catch { return false; }
}
function clientAllowsCoverOptimistic(personal: TableView, covers: Card[], attacks: Card[]): boolean {
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

// Deterministic mapping candidates for the optimistic-cover gate: pair each of
// up to 2 hand cards with each uncovered attack (legal and illegal pairs both
// matter - the gates must AGREE).
function candidateCoverMappings(hand: Card[], b: MemBoard): { covers: Card[]; attacks: Card[] }[] {
  const uncovered = b.battles.filter((x) => !x.defense).map((x) => x.attack);
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

function playAndCheck(np: number, brain: string, stats: { states: number; attacks: number; covers: number }): boolean {
  const t = MemTable.deal(Array.from({ length: np }, (_, i) => ({ id: `bot_${i}`, name: `Bot ${i}`, brain })), dealSeed(), 'acp');
  for (let actions = 0; ; actions++) {
    const game = t.board();
    if (game.status !== L.GAME_STATUS_PLAYING) return true;
    if (actions > MAX_ACTIONS) return false;

    // ---- parity checks on the CURRENT state --------------------------------
    stats.states++;
    for (let seat = 0; seat < game.seats.length; seat++) {
      const p = game.seats[seat];
      if (p.status !== L.PLAYER_STATUS_IN) continue;
      const personal = t.view(seat);

      if (seat !== game.defender) {
        // 1. ATTACK: exact agreement (candidates satisfy the callers'
        //    preconditions: own hand, unique, non-defender)
        for (const cards of candidateAttackSets(p.hand)) {
          const server = serverAllowsAttack(t, seat, cards);
          const button = clientCanAttack(personal, cards);
          const optimistic = clientAllowsAttackOptimistic(personal, cards);
          stats.attacks++;
          const detail = `seat=${seat} first_attacker=${game.firstAttacker} defender=${game.defender} `
            + `table=[${tableText(game)}] cards=[${cards.map(cardKey).join(',')}] seed=${rng.seed}`;
          assert.equal(button, server, `canAttack !== kernel: ${detail}`);
          assert.equal(optimistic, server, `validateAttack !== kernel: ${detail}`);
        }
      } else {
        // 2. COVER button: offered => the move it would send is kernel-legal
        const uncovered = game.battles.filter((b) => !b.defense);
        if (uncovered.length > 0 && p.hand.length > 0) {
          const selections: Card[][] = p.hand.map((c) => [c]);
          if (p.hand.length >= 2) selections.push([p.hand[0], p.hand[1]]);
          for (const sel of selections) {
            // A live Cover button carries a move with it (client_play resolves
            // the button's own target), so this is now one assertion, not two:
            // the move the button would send is one the kernel accepts.
            const move = clientCoverGesture(personal, sel);
            if (!move) continue;
            stats.covers++;
            const covers = [...move.cards], attacks = [...move.attackCards];
            assert.ok(
              serverAllowsCover(t, seat, covers, attacks),
              `client offers a cover the kernel rejects (seed=${rng.seed}): `
              + `covers=[${covers.map(cardKey).join(',')}] attacks=[${attacks.map(cardKey).join(',')}]`,
            );
          }
          // 3. COVER optimistic gate: exact agreement on explicit mappings
          for (const m of candidateCoverMappings(p.hand, game)) {
            const server = serverAllowsCover(t, seat, m.covers, m.attacks);
            const optimistic = clientAllowsCoverOptimistic(personal, m.covers, m.attacks);
            stats.covers++;
            assert.equal(
              optimistic, server,
              `validateCover !== kernel (seed=${rng.seed}): covers=[${m.covers.map(cardKey).join(',')}] `
              + `attacks=[${m.attacks.map(cardKey).join(',')}] table=[${tableText(game)}]`,
            );
          }
        }
      }
    }

    // ---- advance the game: one action of the kernel's bot cycle -------------
    if (t.drive(1).drive.n === 0) return false;
  }
}

if (!process.env.VALIDATION_ONLY) {
  test(`attack/cover parity fuzz: client gates agree with the kernel (${GAMES_PER_PC}/player-count, 2..5 players)`, () => {
    const stats = { states: 0, attacks: 0, covers: 0 };
    let played = 0;
    for (let np = 2; np <= 5; np++) {
      for (let g = 0; g < GAMES_PER_PC; g++) {
        if (playAndCheck(np, g % 2 === 0 ? 'random' : 'handwritten', stats)) played++;
      }
    }
    // eslint-disable-next-line no-console
    console.error(`[attack-cover-parity] games=${played} states=${stats.states} attackChecks=${stats.attacks} coverChecks=${stats.covers}`);
    assert.ok(played > 0, 'no games completed');
    assert.ok(stats.attacks > 0 && stats.covers > 0, 'fuzz exercised both dimensions');
  });
}
