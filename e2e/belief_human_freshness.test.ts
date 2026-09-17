// Guards the resident-belief-log optimization's human-game case (the exact
// concern: "what if a human commits a move mid-drive - does the bot see it?").
//
// The bot loop (bot_actions.ts lockedBotLoop) carries the session log it read
// across cycles ONLY when every seat is a bot, because then the bot lease makes
// the loop the sole writer. The moment a human sits at the table, the loop must
// read games.logs_packed again every cycle the kernel says a belief bot is about
// to choose (table_bots_need_logs), so it picks up the human's committed moves.
// This test drives a REAL human-vs-octogen game on the C Table - the human moves
// through the real move path (executePackedAction), octogen through the real bot
// loop - and asserts that at EVERY octogen decision, its belief log already
// contains EVERY public card the human has committed so far. If the resident
// were (wrongly) reused across a human move, octogen's belief would lag and this
// fails.
//
// OBSERVED FROM THE KERNEL (the belief probe), not from a spy on this side: a spy
// could only prove the loop HANDED the bytes over, never that the importer
// spliced them into the board octogen actually read - which is precisely the gap
// the stale-belief bugs lived in. The probe records the log as the strategy was
// about to read it, on the server's table instance (sdk/ts/table/server_table.ts
// serverTable), which is where the loop's drive (table_bot_drive) runs.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { serverTable } from '../sdk/ts/table/server_table.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { supabaseClient } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { legalMoves, mustReadTable, type PlayCard } from './helpers/table_play.ts';
import { driveBots, runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { ACTION_STATUS } from '../sdk/ts/wire/awire.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Matches the probe's card ids (wasm_belief_probe_dump packs suit*16 + value).
const cid = (c: PlayCard) => `${c.suit}:${c.value}`;

// Bot pacing collapsed, as e2e/helpers/edge.ts does it: the loop's 3 s wait
// between cycles with a human IN is a presentation delay (it cost ~80 s a run).
// The human's chance to move mid-segment is made explicit below instead.
const realSetTimeout = globalThis.setTimeout;
(globalThis as { setTimeout: unknown }).setTimeout = ((fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) =>
  realSetTimeout(fn, (ms ?? 0) >= 250 && (ms ?? 0) <= 5000 ? 0 : ms, ...args)) as unknown as typeof setTimeout;

// One pinned deal: the game, and every octogen search in it, replays exactly.
const DEAL_SEED = Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + 7) & 0xff);

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(() => { __setTableDealSeedOverride(null); });

test('human+octogen: octogen always sees the human\'s committed moves (resident never stale)', async () => {
  const gameId = `hf${uuid().slice(0, 6)}`;
  const humanId = uuid(), botId = uuid();
  // Seat order is the roster order: human = 0, octogen = 1.
  const OCTO_SEAT = 1;
  await seedLobby(gameId, [
    { id: humanId, name: 'Human', ready: false },
    { id: botId, name: 'Octo', brain: 'octogen' },
  ]);
  __setTableDealSeedOverride(DEAL_SEED);
  await runMeta(gameId, humanId, { type: 'start' });
  assert.equal((await mustReadTable(gameId)).status, L.GAME_STATUS_PLAYING, 'fixture: the game dealt');

  // Every public card the human has committed so far (attack/cover/pass cards).
  const humanCards = new Set<string>();
  // Per octogen decision: the set of real cards the KERNEL saw in its belief log,
  // and a snapshot of humanCards at that instant (what it MUST already contain).
  const captures: { belief: Set<string>; expected: Set<string> }[] = [];

  // A human moves WHILE the loop holds its segment, the way a real player does
  // during the loop's wait between cycles: at every lease renewal (the seam
  // between two cycles) the searches so far are recorded against the human
  // cards committed so far, then the human makes a move if one is legal. A loop
  // that reused its resident log across that move would feed the next search a
  // log without it.
  const table = await serverTable();
  const record = () => {
    for (const r of table.__beliefProbeDump().filter((x) => x.seat === OCTO_SEAT)) {
      captures.push({ belief: r.cards, expected: new Set(humanCards) });
    }
    table.__beliefProbeReset();
  };
  let midSegmentMoves = 0;
  const client = supabaseClient as unknown as { rpc: (name: string, params?: Record<string, unknown>) => Promise<unknown> };
  const rpc = client.rpc;
  client.rpc = async (name, params) => {
    if (name === 'renew_bot_lease') {
      record();
      const t = await mustReadTable(gameId);
      const moves = t.status === L.GAME_STATUS_PLAYING ? legalMoves(t, (s) => s.id === humanId).filter((m) => m.kind !== 'good') : [];
      if (moves.length > 0) {
        const pm = moves[midSegmentMoves % moves.length];
        if ((await runAction(gameId, humanId, pm)).status === ACTION_STATUS.APPLIED) {
          for (const c of pm.cards) humanCards.add(cid(c));
          midSegmentMoves++;
        }
      }
    }
    return rpc.call(client, name, params);
  };

  let guard = 0;
  for (let t = await mustReadTable(gameId); t.status === L.GAME_STATUS_PLAYING && ++guard < 60; t = await mustReadTable(gameId)) {
    // A human move to make? (the kernel's enumerator never lists 'wait'.)
    const humanMoves = legalMoves(t, (s) => s.id === humanId);
    if (humanMoves.length > 0) {
      const pm = humanMoves[guard % humanMoves.length];
      const res = await runAction(gameId, humanId, pm);
      assert.equal(res.status, ACTION_STATUS.APPLIED, `the human's legal move applied (step ${guard})`);
      for (const c of pm.cards) humanCards.add(cid(c));
    } else {
      // Octogen's turn - the REAL bot loop (a human is seated, so a fresh log
      // read every belief cycle). Armed per drive segment, and recorded at each
      // cycle seam above and at the segment's end here.
      const before = captures.length;
      table.__beliefProbeReset();
      await driveBots(gameId);
      record();
      if (captures.length === before) break; // bot couldn't act -> avoid spinning
    }
  }

  // Octogen actually chose, and the human actually committed public cards.
  assert.ok(captures.length > 0, 'octogen never chose through the bot loop');
  assert.ok(humanCards.size > 0, 'the human never committed a public card - test never exercised the case');

  // THE GUARD: at every octogen decision, its belief already held every human
  // card committed before it. A stale resident would drop the most recent one.
  let checkedWithHumanCards = 0;
  for (const cap of captures) {
    for (const hc of cap.expected) {
      assert.ok(cap.belief.has(hc),
        `octogen chose with a STALE belief - missing human card ${hc} that was already committed (belief had ${cap.belief.size} cards)`);
    }
    if (cap.expected.size > 0) checkedWithHumanCards++;
  }
  process.stdout.write(`[human-freshness] octogen decisions=${captures.length} (with prior human cards: ${checkedWithHumanCards}) humanCards=${humanCards.size} midSegmentMoves=${midSegmentMoves}\n`);
  assert.ok(checkedWithHumanCards > 0, 'no octogen decision followed a human move - did not actually test freshness');
  assert.ok(midSegmentMoves > 0, 'the human never moved inside a bot-loop segment - the resident log was never put to the test');
  client.rpc = rpc;
});
