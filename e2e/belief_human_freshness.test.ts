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
//
// OBSERVED FROM THE KERNEL (wasmBeliefProbe*), not from a spy on this side.
// Two reasons. The choose step moved in-kernel (bot_drive, F2/A2), so the TS
// seam this used to patch (WasmBotStrategy.chooseMoveDirect) is no longer on
// the bot loop's path at all — patching it captured nothing and the test only
// failed on "octogen never chose". And the stronger reason: a spy here could
// only ever prove the loop HANDED the bytes over, never that the importer
// spliced them into the Game octogen actually read — which is precisely the
// gap the stale-belief bugs lived in. The probe records the log as the
// strategy was about to read it.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { lockedBotLoop } from '../supabase/functions/_shared/adapter/bot_actions.ts';
import { wasmBeliefProbeReset, wasmBeliefProbeDump } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';
import { AnimationEvent, Game, Card } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Matches the probe's card ids (wasm_belief_probe_dump packs suit*16 + value).
const cid = (c: Card) => `${c.suit}:${c.value}`;

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(() => {});

async function loadGame(gameId: string): Promise<Game> {
  const { loadCompleteGame } = await import('../supabase/functions/_shared/adapter/utils.ts');
  return loadCompleteGame(gameId);
}

test('human+octogen: octogen always sees the human’s committed moves (resident never stale)', async () => {
  const gameId = `hf${uuid().slice(0, 6)}`;
  const humanId = uuid(), botId = uuid();
  // Seat order is the players array order: human = 0, octogen = 1.
  const OCTO_SEAT = 1;
  await seedGame(gameId, [
    { id: humanId, name: 'Human', is_ai: false, strategy_key: 'human' },
    { id: botId, name: 'Octo', is_ai: true, strategy_key: 'octogen' },
  ]);
  await executeWithGameLock(gameId,
    async (g: Game) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);

  // Every public card the human has committed so far (attack/cover/pass cards).
  const humanCards = new Set<string>();
  // Per octogen decision: the set of real cards the KERNEL saw in its belief log,
  // and a snapshot of humanCards at that instant (what it MUST already contain).
  const captures: { belief: Set<string>; expected: Set<string> }[] = [];

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
      // Arm per drive segment rather than once for the game: the probe's ring is
      // bounded, and the human cannot move while the loop holds the lease, so
      // every search this records belongs to the humanCards snapshot below.
      wasmBeliefProbeReset();
      await lockedBotLoop(gameId);
      const searches = wasmBeliefProbeDump().filter(r => r.seat === OCTO_SEAT);
      for (const r of searches) captures.push({ belief: r.cards, expected: new Set(humanCards) });
      if (searches.length === 0) break; // bot couldn't act → avoid spinning
    }
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
