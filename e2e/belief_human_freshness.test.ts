// Guards the resident-belief-log optimization's human-game case (the exact
// concern: "what if a human commits a move mid-drive — does the bot see it?").
//
// The resident log is REUSED across cycles only when the game is bots-only
// (humanPlayersStillIn === 0), because then the bot lease makes the loop the
// sole writer. The moment a human is still IN, the loop must reload the session
// log from the DB every cycle so it picks up the human's committed moves. This
// test drives a REAL human-vs-octogen game — the human moves through the real
// commit path, octogen through the real bot loop — and asserts that at EVERY
// octogen decision, its belief log already contains EVERY public card the human
// has committed so far. If the resident were (wrongly) reused across a human
// move, octogen's belief would lag and this fails.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { lockedBotLoop } from '../supabase/functions/_shared/bot_actions.ts';
import { WasmBotStrategy } from '../supabase/functions/_shared/bot_strategy.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';
import { AnimationEvent, Game, Card } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const cid = (c: Card) => `${c.suit}:${c.value}`;
const isReal = (c: Card | null | undefined) => !!c && c.suit >= 0 && c.value >= 1;

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(() => {});

async function loadGame(gameId: string): Promise<Game> {
  const { loadCompleteGame } = await import('../supabase/functions/_shared/utils.ts');
  return loadCompleteGame(gameId);
}

test('human+octogen: octogen always sees the human’s committed moves (resident never stale)', async () => {
  const { decodeLogs } = await import('../supabase/functions/_shared/wire/logwire.ts');

  const gameId = `hf${uuid().slice(0, 6)}`;
  const humanId = uuid(), botId = uuid();
  await seedGame(gameId, [
    { id: humanId, name: 'Human', is_ai: false, strategy_key: 'human' },
    { id: botId, name: 'Octo', is_ai: true, strategy_key: 'octogen' },
  ]);
  await executeWithGameLock(gameId,
    async (g: Game) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);

  // Every public card the human has committed so far (attack/cover/pass cards).
  const humanCards = new Set<string>();
  // Per octogen decision: the set of real cards visible in its belief log, and a
  // snapshot of humanCards at that instant (what it MUST already contain).
  const captures: { belief: Set<string>; expected: Set<string> }[] = [];

  const orig = WasmBotStrategy.prototype.chooseMoveDirect;
  WasmBotStrategy.prototype.chooseMoveDirect = function (game: Game, botPlayerId: string) {
    if ((this as unknown as { logs: boolean }).logs) {
      const belief = new Set<string>();
      const bytes = game.belief_log_bytes;
      if (bytes) {
        try {
          for (const l of decodeLogs(bytes, game.id, game.players)) {
            for (const p of l.card_pairs) {
              if (isReal(p.primary)) belief.add(cid(p.primary));
              if (isReal(p.target)) belief.add(cid(p.target));
            }
          }
        } catch { /* leave empty → will fail the assert if a human card is missing */ }
      }
      captures.push({ belief, expected: new Set(humanCards) });
    }
    return orig.call(this, game, botPlayerId);
  };

  try {
    let guard = 0;
    while (game_done(await loadGame(gameId)) === null && ++guard < 60) {
      const game = await loadGame(gameId);
      // A human move to make? (ignore 'wait' — it commits nothing.)
      const humanMoves = legalMovesFor(game, (id) => id === humanId).filter(pm => pm.move.type !== 'wait');
      if (humanMoves.length > 0) {
        const pm = humanMoves[guard % humanMoves.length];
        for (const c of pm.move.cards ?? []) humanCards.add(cid(c));
        await executeWithGameLock(gameId,
          async (g: Game) => ({ game: g, events: applyPlayerMove(g, pm) }), `h${guard}`, true);
      } else {
        // Octogen's turn — the REAL bot loop (fresh reload each cycle, human IN).
        const before = captures.length;
        await lockedBotLoop(gameId);
        if (captures.length === before) break; // bot couldn't act → avoid spinning
      }
    }
  } finally {
    WasmBotStrategy.prototype.chooseMoveDirect = orig;
  }

  // Octogen actually chose, and the human actually committed public cards.
  assert.ok(captures.length > 0, 'octogen never chose through the bot loop');
  assert.ok(humanCards.size > 0, 'the human never committed a public card — test never exercised the case');

  // THE GUARD: at every octogen decision, its belief already held every human
  // card committed before it. A stale resident would drop the most recent one.
  let checkedWithHumanCards = 0;
  for (const cap of captures) {
    for (const hc of cap.expected) {
      assert.ok(cap.belief.has(hc),
        `octogen chose with a STALE belief — missing human card ${hc} that was already committed (belief had ${cap.belief.size} cards)`);
    }
    if (cap.expected.size > 0) checkedWithHumanCards++;
  }
  console.error(`[human-freshness] octogen decisions=${captures.length} (with prior human cards: ${checkedWithHumanCards}) humanCards=${humanCards.size}`);
  assert.ok(checkedWithHumanCards > 0, 'no octogen decision followed a human move — did not actually test freshness');
});
