// Concurrency regression suite — the seven races from docs/IMESSAGE_GAME_DESIGN.md
// §14 ported to the server-authoritative world (docs/WEB_RACE_BUG_HANDOFF.md §6).
//
// Everything runs the REAL edge modules against a REAL Postgres through the same
// harness the rest of e2e/ uses: executePackedAction (the deployed action path),
// commit_game + the round_epoch guard, the real legal-move enumeration. The only
// thing we simulate is DELIVERY ORDER — the server serializes commits, so a race
// is reproduced by choosing which participant's move we execute first. Each
// stale-round case is run in BOTH orders (pickup-first => the delayed move is
// stale; move-first => it applies and the pickup subsumes it).
//
// The deal is pinned via __setKernelSeedSource so a failure reproduces; the
// per-checkpoint search over fresh games makes "find a state where X races Y"
// robust without hand-crafting a state blob.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { AnimationEvent, Game } from '../server/api/core/types.ts';
import { executePackedAction } from '../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { encodeAction, ACTION_STATUS, REJECT_STALE_ROUND, AwireKindName } from '../sdk/ts/wire/awire.ts';
import { decodeLogs } from '../sdk/ts/wire/logwire.ts';
import { hexToBytes } from '../server/api/common/replay/codec.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { legalMovesFor, checkCardConservation, PlayerMove } from './dispatch.ts';

// Pinned, reproducible deal/RNG stream (see engine.injectDealSeed): a counter
// LCG so successive games get DISTINCT but deterministic deals — the checkpoint
// search below explores real reachable states without crypto non-determinism.
let rngState = 0x1a2b3c4d >>> 0;
const nextSeed = () => { rngState = (rngState * 1664525 + 1013904223) >>> 0; return rngState; };

const wireFor = (pm: PlayerMove) =>
    encodeAction({ kind: pm.move.type as AwireKindName, cards: pm.move.cards, attack_cards: pm.move.attack_cards });

const gamesRow = async (gameId: string) => {
    const r = (await pgPool.query('SELECT version, round_epoch, status FROM games WHERE id=$1', [gameId])).rows[0];
    return { version: Number(r.version ?? 0), roundEpoch: Number(r.round_epoch ?? 0), status: r.status as string };
};

// The committed action log, decoded — used to assert "no move nobody intended
// appears in the round" (a fresh-round attack after a pickup would be the ghost).
async function actionLog(gameId: string): Promise<{ log_type: string; player_id: string | null }[]> {
    const r = (await pgPool.query('SELECT logs_packed, players FROM games WHERE id=$1', [gameId])).rows[0];
    if (!r?.logs_packed) return [];
    const players = (r.players ?? []).map((p: any) => ({ player_id: p.player_id }));
    return decodeLogs(hexToBytes(r.logs_packed), gameId, players).map(l => ({ log_type: l.log_type, player_id: l.player_id }));
}

interface Seeded { gameId: string; players: { id: string; name: string }[] }

async function newPackedGame(humans: number): Promise<Seeded> {
    const gameId = `rc${uuid().slice(0, 6)}`;
    const players = [] as Seeded['players'];
    const seedPlayers = [];
    for (let i = 0; i < humans; i++) {
        const id = uuid();
        players.push({ id, name: `H${i}` });
        seedPlayers.push({ id, name: `H${i}`, is_ai: false, strategy_key: 'human' });
    }
    await seedGame(gameId, seedPlayers);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return { gameId, players };
}

const canPickup = (g: Game, pid: string) => calculateLegalMoves(g, pid).some(m => m.type === 'pickup');

// Drive a real game (packed path) WITHOUT closing the round until a "defender
// deciding, a throw-in is pending" state — the reported race — is reachable, or
// the round can't be advanced further. Returns the pending throw-in (a
// non-defender attack), the defender, and the version the throw-in was composed
// against.
interface Checkpoint { throwIn: PlayerMove; defenderId: string; vCompose: number }
async function driveToThrowIn(gameId: string): Promise<Checkpoint | null> {
    for (let step = 0; step < 40; step++) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== 'playing') return null;
        const defenderId = g.players[g.defender].player_id;
        const throwIns = legalMovesFor(g).filter(pm => pm.move.type === 'attack' && pm.playerId !== defenderId);
        if (canPickup(g, defenderId) && throwIns.length > 0) {
            const { version } = await gamesRow(gameId);
            return { throwIn: throwIns[0], defenderId, vCompose: version };
        }
        // Advance the round without closing it: cover/attack only, never
        // pickup/good (those close it). If nothing else is legal, give up.
        const moves = legalMovesFor(g);
        const progress = moves.find(pm => pm.move.type === 'cover')
            ?? moves.find(pm => pm.move.type === 'attack' || pm.move.type === 'pass');
        if (!progress) return null;
        const out = await executePackedAction(gameId, progress.playerId, wireFor(progress), `drv${step}`);
        if (out.status !== ACTION_STATUS.APPLIED) return null;
    }
    return null;
}

// Find a game + checkpoint by searching fresh deals. Deterministic (pinned RNG).
async function findThrowInGame(humans: number, maxGames = 40): Promise<{ gameId: string; cp: Checkpoint }> {
    for (let i = 0; i < maxGames; i++) {
        const { gameId } = await newPackedGame(humans);
        const cp = await driveToThrowIn(gameId);
        if (cp) return { gameId, cp };
    }
    throw new Error(`no throw-in checkpoint found in ${maxGames} games (pinned seed regression: widen the search)`);
}

if (!process.env.VALIDATION_ONLY) {
before(async () => {
    await applySchema();
    __setKernelSeedSource(() => nextSeed());
});
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { __setKernelSeedSource(null); await pgPool.end(); });

// ---------------------------------------------------------------------------
// Case 1 (the reported bug): defender pickup ∥ attacker throw-in.
// ---------------------------------------------------------------------------
test('case 1: pickup-first — the delayed throw-in is rejected REJECT_STALE_ROUND, never ghost-replays', async () => {
    const { gameId, cp } = await findThrowInGame(2);
    const attackerId = cp.throwIn.playerId;

    // The defender's pickup commits first — it CLOSES the round (takes the table,
    // refills, rotates roles). round_epoch advances past the version the attacker
    // composed the throw-in against.
    const pu = await executePackedAction(gameId, cp.defenderId, encodeAction({ kind: 'pickup' }), 'pickup');
    assert.equal(pu.status, ACTION_STATUS.APPLIED, 'pickup applies');
    const afterPickup = await gamesRow(gameId);
    assert.ok(afterPickup.roundEpoch > cp.vCompose,
        `pickup advanced round_epoch (${afterPickup.roundEpoch}) past the composed version (${cp.vCompose})`);

    const logBefore = (await actionLog(gameId)).length;

    // The in-flight throw-in, composed against the pre-pickup round, arrives now.
    const stale = await executePackedAction(gameId, attackerId, wireFor(cp.throwIn), 'stale', cp.vCompose);
    assert.equal(stale.status, ACTION_STATUS.REJECTED, 'stale throw-in is rejected');
    assert.equal(stale.rejectCode, REJECT_STALE_ROUND, 'rejected specifically as a stale-round move');

    // It never entered the log — no round-N+1 attack that nobody intended.
    const row = await gamesRow(gameId);
    assert.equal(row.version, afterPickup.version, 'a stale-round reject never bumps the version');
    assert.equal((await actionLog(gameId)).length, logBefore, 'the ghost attack is not in the action log');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation after the stale reject: ${chk.detail}`);
});

test('case 1 (other order): move-first — a fresh throw-in applies, and the pickup subsumes it', async () => {
    const { gameId, cp } = await findThrowInGame(2);
    const attackerId = cp.throwIn.playerId;

    // Delivered before the pickup, the throw-in is a legal same-round move — the
    // client composed it against the CURRENT round, so intent == the live version.
    const thrown = await executePackedAction(gameId, attackerId, wireFor(cp.throwIn), 'throw', cp.vCompose);
    assert.equal(thrown.status, ACTION_STATUS.APPLIED, 'a same-round throw-in applies (not stale)');
    assert.notEqual(thrown.rejectCode, REJECT_STALE_ROUND, 'a same-round move is never stale-rejected');

    // The defender still picks up; the round closes cleanly, taking the thrown card.
    const g = await loadCompleteGame(gameId);
    if (canPickup(g, cp.defenderId)) {
        const pu = await executePackedAction(gameId, cp.defenderId, encodeAction({ kind: 'pickup' }), 'pickup');
        assert.equal(pu.status, ACTION_STATUS.APPLIED, 'pickup applies after the throw-in');
    }
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation in the move-first order: ${chk.detail}`);
});

test('the guard is load-bearing: the SAME move on the SAME state differs only by intent version', async () => {
    const { gameId, cp } = await findThrowInGame(2);
    const attackerId = cp.throwIn.playerId;

    // Close the round via pickup.
    await executePackedAction(gameId, cp.defenderId, encodeAction({ kind: 'pickup' }), 'pickup');
    const afterPickup = await gamesRow(gameId);

    // Stale intent (composed against the old round) — REJECTED. A reject mutates
    // nothing, so the very next submission sees the identical state.
    const stale = await executePackedAction(gameId, attackerId, wireFor(cp.throwIn), 'stale', cp.vCompose);
    assert.equal(stale.rejectCode, REJECT_STALE_ROUND, 'stale intent → REJECT_STALE_ROUND');

    // Fresh intent (what a client that SAW the pickup would send) — the guard
    // does NOT fire; the kernel decides. In 2p the picked-on attacker re-opens
    // the new round, so the throw-in card is a legal opening: it APPLIES. Same
    // bytes, same state — only the intent version flipped the outcome, proving
    // the reject is the round guard and not pre-existing kernel behavior (this is
    // exactly the "plays anyway" ghost an old/naive client would have caused).
    const fresh = await executePackedAction(gameId, attackerId, wireFor(cp.throwIn), 'fresh', afterPickup.roundEpoch);
    assert.notEqual(fresh.rejectCode, REJECT_STALE_ROUND, 'fresh intent is never stale-rejected');
    assert.equal(fresh.status, ACTION_STATUS.APPLIED, 'the fresh-intent opening attack applies (the ghost the guard now stops)');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

// ---------------------------------------------------------------------------
// Case 7: same player, two tabs, both act on the same state.
// ---------------------------------------------------------------------------
test('case 7: a cross-round action from a stale tab is rejected; a same-round duplicate follows kernel legality', async () => {
    const { gameId, cp } = await findThrowInGame(2);
    const attackerId = cp.throwIn.playerId;

    // Tab A (fresh) acts: the defender picks up, closing the round.
    await executePackedAction(gameId, cp.defenderId, encodeAction({ kind: 'pickup' }), 'tabA');
    // Tab B still shows the old round and re-fires the throw-in → cross-round → stale.
    const crossRound = await executePackedAction(gameId, attackerId, wireFor(cp.throwIn), 'tabB', cp.vCompose);
    assert.equal(crossRound.rejectCode, REJECT_STALE_ROUND, 'the stale tab is stopped at the round boundary');

    // A SAME-round duplicate is a different animal: play a fresh opening, then
    // replay the identical bytes with fresh intent. The guard passes (same
    // round); the kernel rejects the duplicate (the card already left the hand).
    const g2 = await loadCompleteGame(gameId);
    const open = legalMovesFor(g2).find(pm => pm.move.type === 'attack' && pm.playerId === g2.players[g2.first_attacker].player_id);
    if (open) {
        const v = (await gamesRow(gameId)).version;
        const first = await executePackedAction(gameId, open.playerId, wireFor(open), 'dup1', v);
        assert.equal(first.status, ACTION_STATUS.APPLIED, 'the first opening applies');
        const vAfter = (await gamesRow(gameId)).version;
        const dup = await executePackedAction(gameId, open.playerId, wireFor(open), 'dup2', vAfter);
        assert.notEqual(dup.rejectCode, REJECT_STALE_ROUND, 'a same-round duplicate is NOT a stale-round reject');
        // It's a kernel rejection (card no longer in hand) — a normal, non-stale reject.
        assert.equal(dup.status, ACTION_STATUS.REJECTED, 'the duplicate is kernel-rejected');
    }
});

// ---------------------------------------------------------------------------
// Cases 2/3/4: legitimate SAME-round concurrency must keep working (never
// stale-rejected). These are the moves the doc warns a version-scoped guard
// would wrongly kill; the round-scoped guard must leave them untouched.
// ---------------------------------------------------------------------------
test('case 2: two attackers throw in simultaneously (3p) — both same-round, neither stale-rejected', async () => {
    // Reach a state with TWO distinct non-defender attackers holding a legal
    // attack against the same table.
    let found: { gameId: string; a: PlayerMove; b: PlayerMove; v: number } | null = null;
    for (let i = 0; i < 40 && !found; i++) {
        const { gameId } = await newPackedGame(3);
        for (let step = 0; step < 40; step++) {
            const g = await loadCompleteGame(gameId);
            if (g.status !== 'playing') break;
            const defId = g.players[g.defender].player_id;
            const attacks = legalMovesFor(g).filter(pm => pm.move.type === 'attack' && pm.playerId !== defId);
            const byPlayer = new Map<string, PlayerMove>();
            for (const pm of attacks) if (!byPlayer.has(pm.playerId)) byPlayer.set(pm.playerId, pm);
            if (byPlayer.size >= 2) {
                const [a, b] = [...byPlayer.values()];
                found = { gameId, a, b, v: (await gamesRow(gameId)).version };
                break;
            }
            const progress = legalMovesFor(g).find(pm => pm.move.type === 'cover' || pm.move.type === 'attack');
            if (!progress) break;
            const out = await executePackedAction(gameId, progress.playerId, wireFor(progress), `drv${step}`);
            if (out.status !== ACTION_STATUS.APPLIED) break;
        }
    }
    assert.ok(found, 'reached a two-attacker state');
    const { gameId, a, b, v } = found!;
    // Both composed against the same live round → both carry the current version.
    const r1 = await executePackedAction(gameId, a.playerId, wireFor(a), 'atk1', v);
    assert.equal(r1.status, ACTION_STATUS.APPLIED, 'first throw-in applies');
    assert.notEqual(r1.rejectCode, REJECT_STALE_ROUND, 'not stale');
    const r2 = await executePackedAction(gameId, b.playerId, wireFor(b), 'atk2', v);
    // Accepted if capacity allows; if the first consumed the last slot the second
    // is a CAPACITY reject — but NEVER a stale-round reject (same round).
    assert.notEqual(r2.rejectCode, REJECT_STALE_ROUND, 'the second throw-in is not stale-rejected (same round)');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

test('case 3: defender cover ∥ attacker adds a battle — both same-round, no stale reject in either order', async () => {
    // A state where the defender can cover AND a non-defender can add an attack.
    let found: { gameId: string; cover: PlayerMove; attack: PlayerMove; v: number } | null = null;
    for (let i = 0; i < 40 && !found; i++) {
        const { gameId } = await newPackedGame(3);
        for (let step = 0; step < 40; step++) {
            const g = await loadCompleteGame(gameId);
            if (g.status !== 'playing') break;
            const defId = g.players[g.defender].player_id;
            const cover = legalMovesFor(g).find(pm => pm.move.type === 'cover' && pm.playerId === defId);
            const attack = legalMovesFor(g).find(pm => pm.move.type === 'attack' && pm.playerId !== defId);
            if (cover && attack) {
                found = { gameId, cover, attack, v: (await gamesRow(gameId)).version };
                break;
            }
            const progress = legalMovesFor(g).find(pm => pm.move.type === 'attack');
            if (!progress) break;
            const out = await executePackedAction(gameId, progress.playerId, wireFor(progress), `drv${step}`);
            if (out.status !== ACTION_STATUS.APPLIED) break;
        }
    }
    assert.ok(found, 'reached a cover∥attack state');
    const { gameId, cover, attack, v } = found!;
    const rc = await executePackedAction(gameId, cover.playerId, wireFor(cover), 'cover', v);
    assert.notEqual(rc.rejectCode, REJECT_STALE_ROUND, 'cover not stale');
    const ra = await executePackedAction(gameId, attack.playerId, wireFor(attack), 'attack', v);
    assert.notEqual(ra.rejectCode, REJECT_STALE_ROUND, 'the added attack not stale (same round)');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

// ---------------------------------------------------------------------------
// Case 6: action ∥ game-ending move — the delayed action against a finished
// game is MOOT (the pre-existing game-over guard), never a spurious error.
// ---------------------------------------------------------------------------
test('case 6: a move delivered after the game ended is MOOT (no post-terminal mutation)', async () => {
    const { gameId, players } = await newPackedGame(2);
    const before = await gamesRow(gameId);
    await pgPool.query(`UPDATE games SET status='game_over' WHERE id=$1`, [gameId]);
    __clearGameCache();
    const out = await executePackedAction(gameId, players[0].id, encodeAction({ kind: 'good' }), 'late', before.version);
    assert.equal(out.status, ACTION_STATUS.MOOT, 'a move against a finished game is a no-op, not an error');
    const after = await gamesRow(gameId);
    assert.equal(after.version, before.version, 'no post-terminal version bump');
});

// ---------------------------------------------------------------------------
// Suite invariant: an old client (v1 envelope, no intent version) is never
// stale-rejected — the guard tightens only for clients that opt in, so a
// rollout can never break a legitimate move.
// ---------------------------------------------------------------------------
test('backward compat: a move without an intent version is never stale-rejected across a round close', async () => {
    const { gameId, cp } = await findThrowInGame(2);
    await executePackedAction(gameId, cp.defenderId, encodeAction({ kind: 'pickup' }), 'pickup');
    // No intentVersion — the v1 wire. Across the same round close that rejects a
    // v2 client, this is NOT stale-rejected (it reaches the kernel — today's
    // behavior, preserved for the rollout window).
    const out = await executePackedAction(gameId, cp.throwIn.playerId, wireFor(cp.throwIn), 'v1');
    assert.notEqual(out.rejectCode, REJECT_STALE_ROUND, 'a v1 (no-intent) move is not stale-rejected');
});
}
