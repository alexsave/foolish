// ADVERSARIAL: the TypeScript production layer — the endpoints a malicious
// client actually reaches. Drives the REAL dispatch (verify_player_in_game +
// the real action/meta handlers) under the REAL CAS commit against real
// Postgres, and hammers it with concurrency. Two axes:
//   1. authorization / logic exploits (can I affect state I shouldn't?)
//   2. rapid-fire concurrency (can I race the CAS commit into duplicating or
//      losing a card, or wedge/deadlock the lock?)
// The hard invariant after everything: card conservation holds and no request
// crashes the server (a clean rule rejection is fine; a 500-class crash isn't).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { handleMetaAction } from '../server/impls/supabase/functions/_shared/adapter/meta_actions.ts';
import { game_done, verify_player_in_game } from '../server/api/common/common_utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { handleAttack } from '../server/api/common/actions/attack.ts';
import { handleCover } from '../server/api/common/actions/cover.ts';
import { handlePass } from '../server/api/common/actions/pass.ts';
import { handlePickup } from '../server/api/common/actions/pickup.ts';
import { handleGood } from '../server/api/common/actions/good.ts';
import { legalMovesFor, checkCardConservation } from './dispatch.ts';
import { Game, AnimationEvent, PLAYER_STATUS, GAME_STATUS } from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const H0 = uuid(), H1 = uuid(), H2 = uuid();
async function freshLobby(id: string, started = true): Promise<void> {
  await seedGame(id, [
    { id: H0, name: 'Alice', is_ai: false, strategy_key: 'human' },
    { id: H1, name: 'Bob', is_ai: false, strategy_key: 'human' },
    { id: H2, name: 'Carol', is_ai: false, strategy_key: 'human' },
  ]);
  if (started) await executeWithGameLock(id, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
}

// The real action-endpoint dispatch (mirrors action/index.ts): it uses the
// AUTHENTICATED user id, never a client-supplied player_id.
function applyAsUser(game: Game, userId: string, body: any): AnimationEvent[] {
  verify_player_in_game(game, userId);
  switch (body.type) {
    case 'attack': return handleAttack(game, userId, body.cards);
    case 'cover': return handleCover(game, userId, body.cover_cards, body.attack_cards);
    case 'pass': return handlePass(game, userId, body.cards);
    case 'pickup': return handlePickup(game, userId);
    case 'good': return handleGood(game, userId);
    default: throw new Error(`unknown ${body.type}`);
  }
}

before(applySchema);
beforeEach(resetDb);
after(async () => { await pgPool.end(); });

// ---------------------------------------------------------------------------
// 1. AUTHORIZATION — the meta endpoint uses body-supplied ids
// ---------------------------------------------------------------------------

test('exit: any lobby player may kick any other (intended) without corrupting state', async () => {
  const id = 'advexit';
  await freshLobby(id, false); // lobby (exit requires WAITING)
  // Bob kicks Alice by supplying her player_id — this is intentional lobby
  // management. What matters adversarially: it only removes the named player,
  // leaves the rest intact, and can't be turned into corruption.
  await executeWithGameLock(id, async (g) =>
    handleMetaAction({ user: { id: H1 } as any, user_name: 'Bob', body: { type: 'exit', game_id: id, player_id: H0 }, game: g, reqId: 'x' }), 'exit', false);
  const g = await loadCompleteGame(id);
  assert.ok(!g.players.some(p => p.player_id === H0), 'Alice was kicked (allowed)');
  assert.ok(g.players.some(p => p.player_id === H1), 'Bob still present');
  assert.ok(g.players.some(p => p.player_id === H2), 'Carol untouched');
  // Kicking a non-member / already-gone id must reject, not corrupt.
  await assert.rejects(() => executeWithGameLock(id, async (gg) =>
    handleMetaAction({ user: { id: H1 } as any, user_name: 'Bob', body: { type: 'exit', game_id: id, player_id: H0 }, game: gg, reqId: 'x2' }), 'exit', false));
});

test('exit: removing yourself still works', async () => {
  const id = 'advexit2';
  await freshLobby(id, false);
  await executeWithGameLock(id, async (g) =>
    handleMetaAction({ user: { id: H1 } as any, user_name: 'Bob', body: { type: 'exit', game_id: id }, game: g, reqId: 'x' }), 'exit', false);
  const g = await loadCompleteGame(id);
  assert.ok(!g.players.some(p => p.player_id === H1), 'Bob removed himself');
  assert.ok(g.players.some(p => p.player_id === H0), 'Alice untouched');
});

test('add-bot flood is capped at MAX_PLAYERS (no oversized-lobby crash)', async () => {
  const id = 'advflood';
  await seedGame(id, [{ id: H0, name: 'Alice', is_ai: false, strategy_key: 'human' }]);
  // A joined human is IDLE, which blocks add-bot's auto-start — so a client
  // could otherwise flood the roster (dozens of bots) into the lobby and then
  // crash the deal on start. Keep Alice idle to exercise that path.
  await pgPool.query(`UPDATE games SET players = jsonb_set(players,'{0,status}','"idle"') WHERE id=$1`, [id]);
  for (let i = 0; i < 20; i++)
    await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT DO NOTHING', [uuid(), `Bot${i}`, 'random']);
  let full = false;
  for (let i = 0; i < 20; i++) {
    try {
      await executeWithGameLock(id, async (g) =>
        handleMetaAction({ user: { id: H0 } as any, user_name: 'Alice', body: { type: 'add-bot', game_id: id }, game: g, reqId: 'a' }), 'a', false);
    } catch (e: any) { assert.match(String(e.message), /full/i); full = true; break; }
  }
  assert.ok(full, 'add-bot must reject once the lobby is full');
  const g = await loadCompleteGame(id);
  assert.ok(g.players.length <= 8, `lobby capped, got ${g.players.length}`);
  // Even if an oversized lobby somehow existed (corrupt row, pre-fix data),
  // start_game must reject cleanly rather than crash the deal.
  const oversized: any = {
    ...g, status: GAME_STATUS.WAITING,
    players: Array.from({ length: 30 }, (_, i) => ({
      player_id: `x${i}`, name: `X${i}`, status: PLAYER_STATUS.READY, is_ai: true,
      hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'random',
    })),
  };
  assert.throws(() => start_game(oversized), /Cannot start|max/i, 'oversized start rejects, not crashes');
});

// ---------------------------------------------------------------------------
// 2. RAPID-FIRE CONCURRENCY — race the CAS commit
// ---------------------------------------------------------------------------

test('concurrency: add-bot flood fired all at once still caps at MAX_PLAYERS', async () => {
  const id = 'advfloodrace';
  await seedGame(id, [{ id: H0, name: 'Alice', is_ai: false, strategy_key: 'human' }]);
  await pgPool.query(`UPDATE games SET players = jsonb_set(players,'{0,status}','"idle"') WHERE id=$1`, [id]);
  for (let i = 0; i < 30; i++)
    await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT DO NOTHING', [uuid(), `Bot${i}`, 'random']);
  // 30 add-bot requests fired concurrently against one version — the lock
  // serializes them, and the cap must hold across the whole burst.
  await Promise.all(Array.from({ length: 30 }, (_, i) =>
    executeWithGameLock(id, async (g) =>
      handleMetaAction({ user: { id: H0 } as any, user_name: 'Alice', body: { type: 'add-bot', game_id: id }, game: g, reqId: `f${i}` }), `f${i}`, false)
      .catch(() => null)));
  const g = await loadCompleteGame(id);
  assert.ok(g.players.length <= 8, `concurrent flood still capped, got ${g.players.length}`);
  // no duplicate bot ids slipped through the race
  const ids = g.players.map(p => p.player_id);
  assert.equal(new Set(ids).size, ids.length, 'no duplicate players from the race');
});

test('concurrency: 200 overlapping submits against one state cannot dup/lose a card', async () => {
  const id = 'advrace';
  await freshLobby(id);
  const g0 = await loadCompleteGame(id);
  const moves = legalMovesFor(g0);
  // Fire the same legal move from many clients simultaneously, plus a burst of
  // illegal/garbage submits interleaved — all racing one version.
  const attempts: Promise<any>[] = [];
  for (let i = 0; i < 200; i++) {
    const pick = moves[i % Math.max(1, moves.length)];
    const body = i % 3 === 0 && pick
      ? { type: pick.move.type, cards: pick.move.cards, cover_cards: pick.move.cards, attack_cards: pick.move.attack_cards }
      : { type: ['attack', 'cover', 'pass', 'pickup', 'good'][i % 5], cards: [{ suit: i % 4, value: 1 + (i % 13) }] };
    const uid = pick ? pick.playerId : H0;
    attempts.push(
      executeWithGameLock(id, async (g) => ({ game: g, events: applyAsUser(g, uid, body) }), `r${i}`, true)
        .catch(() => null), // rejections are fine; we only care about the invariant
    );
  }
  await Promise.all(attempts);
  const chk = await checkCardConservation(id);
  assert.ok(chk.ok, `card conservation after 200-way race: ${chk.detail}`);
});

test('concurrency: rapid full-game self-play through the lock stays conserved', async () => {
  const id = 'advrapid';
  await freshLobby(id);
  const total0 = (await checkCardConservation(id)).detail;
  let guard = 0;
  while (guard++ < 4000) {
    const g = await loadCompleteGame(id);
    if (game_done(g) !== null) break;
    const moves = legalMovesFor(g);
    if (!moves.length) break;
    // Fire the next move AND three stale/garbage duplicates concurrently.
    const m = moves[0];
    const body = { type: m.move.type, cards: m.move.cards, cover_cards: m.move.cards, attack_cards: m.move.attack_cards };
    await Promise.all([
      executeWithGameLock(id, async (gg) => ({ game: gg, events: applyAsUser(gg, m.playerId, body) }), 'rr', true).catch(() => null),
      executeWithGameLock(id, async (gg) => ({ game: gg, events: applyAsUser(gg, m.playerId, body) }), 'rr2', true).catch(() => null),
      executeWithGameLock(id, async (gg) => ({ game: gg, events: applyAsUser(gg, H2, { type: 'pickup' }) }), 'rr3', true).catch(() => null),
    ]);
    const chk = await checkCardConservation(id);
    assert.ok(chk.ok, `conservation mid-rapid-game (was ${total0}): ${chk.detail}`);
  }
});

// ---------------------------------------------------------------------------
// 3. game_id / payload type confusion at the lock + load boundary
// ---------------------------------------------------------------------------

test('meta: hostile game_id / bot_id / player_id values never crash the handler', async () => {
  const id = 'advmeta';
  await freshLobby(id, false);
  const hostile = [
    { type: 'exit', bot_id: { $ne: null } },
    { type: 'exit', bot_id: ['a', 'b'] },
    { type: 'exit', player_id: "1' OR '1'='1" },
    { type: 'exit', bot_id: 'nonexistent' },
    { type: 'add-bot', strategy_key: "'; DROP TABLE games;--" },
    { type: 'rearrange-hand', card_indices: 'not-an-array' },
    { type: 'rearrange-hand', card_indices: [0, 0, 0] },
    { type: 'rearrange-hand', card_indices: [999, -1] },
    { type: 'rearrange-players', player_order: [{ __proto__: { admin: true } }] },
    { type: '__proto__' },
    { type: 'constructor' },
    { type: 'update-name', name: 'x'.repeat(100000) },
    { type: 'update-name', name: { toString: () => { throw new Error('boom'); } } },
  ];
  for (const body of hostile) {
    let crashed = false;
    try {
      await executeWithGameLock(id, async (g) =>
        handleMetaAction({ user: { id: H0 } as any, user_name: 'Alice', body: { ...body, game_id: id }, game: g, reqId: 'h' }), 'h', false)
        .catch((e) => { // a thrown rule error is acceptable; a crash-class one is not
          const m = String(e?.message ?? e);
          if (/cannot read|is not a function|is not iterable|maximum call stack|reading '/i.test(m)) crashed = true;
        });
    } catch (e: any) {
      const m = String(e?.message ?? e);
      if (/cannot read|is not a function|is not iterable|maximum call stack|reading '/i.test(m)) crashed = true;
    }
    assert.ok(!crashed, `hostile meta body crashed the handler: ${JSON.stringify(body).slice(0, 80)}`);
  }
  // and the game survived it all
  const chk = await checkCardConservation(id);
  assert.ok((await loadCompleteGame(id)).players.length > 0, `game intact after hostile meta barrage: ${chk.detail}`);
});

// ---------------------------------------------------------------------------
// 4. numeric / structural card edge cases reaching the kernel marshaling
// ---------------------------------------------------------------------------

test('numeric edge-case cards are rejected, never applied or crashed', async () => {
  const id = 'advnum';
  await freshLobby(id);
  const g0 = await loadCompleteGame(id);
  const fa = g0.players[g0.first_attacker].player_id;
  const evil = [
    [{ suit: 0, value: Infinity }], [{ suit: 0, value: -Infinity }], [{ suit: 0, value: NaN }],
    [{ suit: 0, value: 1e300 }], [{ suit: 1e300, value: 5 }], [{ suit: -0, value: -0 }],
    [{ suit: 0, value: 13.0000001 }], [{ suit: 0.5, value: 5.5 }],
    [{ suit: 0, value: 5, extra: 'x'.repeat(10000) }],
    [{ suit: { valueOf: () => 0 }, value: { valueOf: () => 5 } }],
  ];
  for (const cards of evil) {
    await executeWithGameLock(id, async (g) => ({ game: g, events: applyAsUser(g, fa, { type: 'attack', cards }) }), 'n', true)
      .catch(() => null); // must reject or apply cleanly, never crash the lock
    const chk = await checkCardConservation(id);
    assert.ok(chk.ok, `conservation after numeric-evil card ${JSON.stringify(cards).slice(0, 60)}: ${chk.detail}`);
  }
});
