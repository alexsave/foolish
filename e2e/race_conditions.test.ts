// Concurrency regression suite - the seven races from docs/IMESSAGE_GAME_DESIGN.md
// §14 ported to the server-authoritative world (docs/WEB_RACE_BUG_HANDOFF.md §6),
// on the C Table (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b, risk 5.2).
//
// Everything runs the REAL edge modules against a REAL Postgres through the same
// harness the rest of e2e/ uses: executePackedAction (the deployed action path),
// the table_io CAS loop, commit_table and its round_epoch stamp, table_act's round
// guard. Most cases simulate DELIVERY ORDER - the server serializes commits, so a
// race is reproduced by choosing which participant's move executes first, and
// each stale-round case runs in BOTH orders (pickup-first => the delayed move is
// stale; move-first => it applies and the pickup subsumes it). The last cases
// race for real: requests in flight together, this isolate's row cache going
// stale under another isolate's commit, and two games' operations interleaving
// on the module's one resident table across an await.
//
// Boards are built and sealed by the kernel (helpers/table_fixture.ts), so the
// state each race needs is written down instead of searched for.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { cardText, checkCardConservation, legalMoves, mustReadTable, type PlayMove, type TableState } from './helpers/table_play.ts';
import { runAction, runMeta } from './helpers/table_server.ts';
import { __clearGameCache, getCachedRow, noteCommittedRow } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { encodeAction, ACTION_STATUS, REJECT_STALE_ROUND } from '../sdk/ts/wire/awire.ts';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const PICKUP = encodeAction({ kind: 'pickup' });
const hexOf = (b: Uint8Array) => Buffer.from(b).toString('hex');
const hand = (t: TableState, seat: number) => t.seats[seat].hand.map(cardText);
const settle = () => new Promise((r) => setImmediate(() => setImmediate(r)));

/** The legal move of `seat` of this kind playing exactly `cards` (card text), or a failed assertion. */
function moveOf(t: TableState, seat: number, kind: PlayMove['kind'], cards: string): PlayMove {
    const want = cards.split(' ').filter(Boolean).join(' ');
    const m = legalMoves(t, (_, s) => s === seat).find((x) => x.kind === kind && x.cards.map(cardText).join(' ') === want);
    assert.ok(m, `fixture: seat ${seat} can ${kind} ${cards}`);
    return m!;
}

// The version every board is seeded at, so a round closed on it stamps a
// round_epoch the moves composed at V0 predate.
const V0 = 5;

interface Two { gameId: string; A: string; B: string }
interface Three extends Two { C: string }

/**
 * 2p, one attack down: A (seat 0) opened 6h and can throw in 6d; B (seat 1)
 * defends and may cover 6h with 7h or pick up. Round_epoch 0, version V0.
 */
async function throwInGame(prefix = 'rc'): Promise<Two> {
    const gameId = `${prefix}${uuid().slice(0, 6)}`;
    const A = uuid(), B = uuid();
    await seedTable(gameId, fixture()
        .seats([{ id: A, name: 'A' }, { id: B, name: 'B' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6d 7c 8c 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').table('6h')
        .deck('Ks Qs Js Ts 9s 8s').trump('As').discard(17)
        .build(), { version: V0 });
    return { gameId, A, B };
}

/**
 * 3p, one attack down: A (seat 0) opened 6h; B (seat 1) defends and can cover
 * with 7h; A holds 6d and C (seat 2) holds 6c, both legal throw-ins.
 */
async function threeSeatGame(): Promise<Three> {
    const gameId = `r3${uuid().slice(0, 6)}`;
    const A = uuid(), B = uuid(), C = uuid();
    await seedTable(gameId, fixture()
        .seats([{ id: A, name: 'A' }, { id: B, name: 'B' }, { id: C, name: 'C' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6d 7c 8c 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').hand(2, '6c 7s 8s Ad Kd Qd').table('6h')
        .deck('Ks Qs Js Ts 9s 8d').trump('As').discard(11)
        .build(), { version: V0 });
    return { gameId, A, B, C };
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await settle(); await pgPool.end(); });

// ---------------------------------------------------------------------------
// Case 1 (the reported bug): defender pickup ∥ attacker throw-in.
// ---------------------------------------------------------------------------
test('case 1: pickup-first - the delayed throw-in is rejected REJECT_STALE_ROUND, never ghost-replays', async () => {
    const { gameId, A, B } = await throwInGame();
    const t0 = await mustReadTable(gameId);
    const throwIn = moveOf(t0, 0, 'attack', '6d');

    // The defender's pickup commits first - it CLOSES the round (takes the table,
    // refills, rotates roles). round_epoch advances past the version the attacker
    // composed the throw-in against.
    const pu = await runAction(gameId, B, PICKUP, V0);
    assert.equal(pu.status, ACTION_STATUS.APPLIED, 'pickup applies');
    const afterPickup = await mustReadTable(gameId);
    assert.ok(afterPickup.roundEpoch > V0,
        `pickup advanced round_epoch (${afterPickup.roundEpoch}) past the composed version (${V0})`);

    // The in-flight throw-in, composed against the pre-pickup round, arrives now.
    const stale = await runAction(gameId, A, throwIn, V0);
    assert.equal(stale.status, ACTION_STATUS.REJECTED, 'stale throw-in is rejected');
    assert.equal(stale.rejectCode, REJECT_STALE_ROUND, 'rejected specifically as a stale-round move');

    // It never entered the log - no round-N+1 attack that nobody intended.
    const row = await mustReadTable(gameId);
    assert.equal(row.version, afterPickup.version, 'a stale-round reject never bumps the version');
    assert.equal(row.logsPacked, afterPickup.logsPacked, 'the ghost attack is not in the session log');
    assert.equal(hexOf(row.state), hexOf(afterPickup.state), 'nor on the board');
    assert.ok(hand(row, 0).includes('6d'), 'the throw-in card never left the attacker\'s hand');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation after the stale reject: ${chk.detail}`);
});

test('case 1 (other order): move-first - a fresh throw-in applies, and the pickup subsumes it', async () => {
    const { gameId, A, B } = await throwInGame();
    const throwIn = moveOf(await mustReadTable(gameId), 0, 'attack', '6d');

    // Delivered before the pickup, the throw-in is a legal same-round move - the
    // client composed it against the CURRENT round, so intent == the live version.
    const thrown = await runAction(gameId, A, throwIn, V0);
    assert.equal(thrown.status, ACTION_STATUS.APPLIED, 'a same-round throw-in applies (not stale)');
    assert.notEqual(thrown.rejectCode, REJECT_STALE_ROUND, 'a same-round move is never stale-rejected');

    // The defender still picks up; the round closes cleanly, taking the thrown card.
    const pu = await runAction(gameId, B, PICKUP, V0);
    assert.equal(pu.status, ACTION_STATUS.APPLIED, 'pickup applies after the throw-in');
    const t = await mustReadTable(gameId);
    assert.ok(hand(t, 1).includes('6d') && hand(t, 1).includes('6h'), 'the defender took the thrown card too');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation in the move-first order: ${chk.detail}`);
});

test('the guard is load-bearing: the SAME move on the SAME state differs only by intent version', async () => {
    const { gameId, A, B } = await throwInGame();
    const throwIn = moveOf(await mustReadTable(gameId), 0, 'attack', '6d');

    // Close the round via pickup.
    await runAction(gameId, B, PICKUP, V0);
    const afterPickup = await mustReadTable(gameId);

    // Stale intent (composed against the old round) - REJECTED. A reject mutates
    // nothing, so the very next submission sees the identical state.
    const stale = await runAction(gameId, A, throwIn, V0);
    assert.equal(stale.rejectCode, REJECT_STALE_ROUND, 'stale intent -> REJECT_STALE_ROUND');

    // Fresh intent (what a client that SAW the pickup would send) - the guard
    // does NOT fire; the kernel decides. In 2p the picked-on attacker re-opens
    // the new round, so the throw-in card is a legal opening: it APPLIES. Same
    // bytes, same state - only the intent version flipped the outcome, proving
    // the reject is the round guard and not pre-existing kernel behavior (this is
    // exactly the "plays anyway" ghost an old/naive client would have caused).
    const fresh = await runAction(gameId, A, throwIn, afterPickup.roundEpoch);
    assert.notEqual(fresh.rejectCode, REJECT_STALE_ROUND, 'fresh intent is never stale-rejected');
    assert.equal(fresh.status, ACTION_STATUS.APPLIED, 'the fresh-intent opening attack applies (the ghost the guard now stops)');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

// ---------------------------------------------------------------------------
// Case 7: same player, two tabs, both act on the same state.
// ---------------------------------------------------------------------------
test('case 7: a cross-round action from a stale tab is rejected; a same-round duplicate follows kernel legality', async () => {
    const { gameId, A, B } = await throwInGame();
    const throwIn = moveOf(await mustReadTable(gameId), 0, 'attack', '6d');

    // Tab A (fresh) acts: the defender picks up, closing the round.
    await runAction(gameId, B, PICKUP, V0);
    // Tab B still shows the old round and re-fires the throw-in -> cross-round -> stale.
    const crossRound = await runAction(gameId, A, throwIn, V0);
    assert.equal(crossRound.rejectCode, REJECT_STALE_ROUND, 'the stale tab is stopped at the round boundary');

    // A SAME-round duplicate is a different animal: play a fresh opening, then
    // replay the identical bytes with fresh intent. The guard passes (same
    // round); the kernel rejects the duplicate (the card already left the hand).
    const t = await mustReadTable(gameId);
    const open = legalMoves(t, (_, s) => s === t.firstAttacker).find((m) => m.kind === 'attack' && m.cards.length === 1);
    assert.ok(open, 'the new round has an opening attack');
    const first = await runAction(gameId, open!.playerId, open!, t.version);
    assert.equal(first.status, ACTION_STATUS.APPLIED, 'the first opening applies');
    const dup = await runAction(gameId, open!.playerId, open!, first.version);
    assert.notEqual(dup.rejectCode, REJECT_STALE_ROUND, 'a same-round duplicate is NOT a stale-round reject');
    // It's a kernel rejection (card no longer in hand) - a normal, non-stale reject.
    assert.equal(dup.status, ACTION_STATUS.REJECTED, 'the duplicate is kernel-rejected');
    assert.equal(dup.rejectCode, L.ENGINE_REJECT_NOT_IN_HAND, 'because the card already left the hand');
});

// ---------------------------------------------------------------------------
// Cases 2/3/4: legitimate SAME-round concurrency must keep working (never
// stale-rejected). These are the moves the doc warns a version-scoped guard
// would wrongly kill; the round-scoped guard must leave them untouched.
// ---------------------------------------------------------------------------
test('case 2: two attackers throw in simultaneously (3p) - both same-round, neither stale-rejected', async () => {
    const { gameId, A, C } = await threeSeatGame();
    const t = await mustReadTable(gameId);
    const a = moveOf(t, 0, 'attack', '6d');
    const c = moveOf(t, 2, 'attack', '6c');
    // Both composed against the same live round -> both carry the current version.
    const r1 = await runAction(gameId, A, a, V0);
    assert.equal(r1.status, ACTION_STATUS.APPLIED, 'first throw-in applies');
    assert.notEqual(r1.rejectCode, REJECT_STALE_ROUND, 'not stale');
    const r2 = await runAction(gameId, C, c, V0);
    // Accepted if capacity allows; if the first consumed the last slot the second
    // is a CAPACITY reject - but NEVER a stale-round reject (same round).
    assert.notEqual(r2.rejectCode, REJECT_STALE_ROUND, 'the second throw-in is not stale-rejected (same round)');
    assert.equal(r2.status, ACTION_STATUS.APPLIED, 'and here, with room on the table, it applies');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

test('case 3: defender cover ∥ attacker adds a battle - both same-round, no stale reject in either order', async () => {
    for (const coverFirst of [true, false]) {
        const { gameId, B, C } = await threeSeatGame();
        const t = await mustReadTable(gameId);
        const cover = moveOf(t, 1, 'cover', '7h');
        const attack = moveOf(t, 2, 'attack', '6c');
        const order: [string, PlayMove][] = coverFirst ? [[B, cover], [C, attack]] : [[C, attack], [B, cover]];
        for (const [who, m] of order) {
            const r = await runAction(gameId, who, m, V0);
            assert.notEqual(r.rejectCode, REJECT_STALE_ROUND, `${m.kind} not stale (${coverFirst ? 'cover' : 'attack'} first)`);
            assert.equal(r.status, ACTION_STATUS.APPLIED, `${m.kind} applies (${coverFirst ? 'cover' : 'attack'} first)`);
        }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation: ${chk.detail}`);
    }
});

// ---------------------------------------------------------------------------
// Case 6: action ∥ game-ending move - the delayed action against a finished
// game is MOOT (the game-over guard outranks the round guard), never an error.
// ---------------------------------------------------------------------------
test('case 6: a move delivered after the game ended is MOOT (no post-terminal mutation)', async () => {
    const gameId = `re${uuid().slice(0, 6)}`;
    const A = uuid(), B = uuid();
    await seedTable(gameId, fixture().seats([{ id: A, name: 'A' }, { id: B, name: 'B' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6h').hand(1, '7s 8s').powerSuit(L.SUIT_CLUBS).discard(33).build(), { version: V0 });
    const last = moveOf(await mustReadTable(gameId), 0, 'attack', '6h');
    assert.equal((await runAction(gameId, A, last, V0)).status, ACTION_STATUS.APPLIED, 'A\'s last card ends the game');
    const ended = await mustReadTable(gameId);
    assert.equal(ended.statusColumn, 'game_over', 'the game is over');

    const out = await runAction(gameId, B, PICKUP, V0);
    assert.equal(out.status, ACTION_STATUS.MOOT, 'a move against a finished game is a no-op, not an error');
    const row = await mustReadTable(gameId);
    assert.equal(row.version, ended.version, 'no post-terminal version bump');
    assert.equal(hexOf(row.state), hexOf(ended.state), 'no post-terminal mutation');
});

// ---------------------------------------------------------------------------
// Suite invariant: an old client (v1 envelope, no intent version) is never
// stale-rejected - the guard tightens only for clients that opt in, so a
// rollout can never break a legitimate move.
// ---------------------------------------------------------------------------
test('backward compat: a move without an intent version is never stale-rejected across a round close', async () => {
    const { gameId, A, B } = await throwInGame();
    const throwIn = moveOf(await mustReadTable(gameId), 0, 'attack', '6d');
    await runAction(gameId, B, PICKUP);
    // No intent version - the v1 wire. Across the same round close that rejects a
    // v2 client, this is NOT stale-rejected (it reaches the kernel).
    const out = await runAction(gameId, A, throwIn);
    assert.notEqual(out.rejectCode, REJECT_STALE_ROUND, 'a v1 (no-intent) move is not stale-rejected');
});

// ---------------------------------------------------------------------------
// Real concurrency: requests in flight together.
// ---------------------------------------------------------------------------
test('in flight together: pickup ∥ throw-in resolve to one of the two serial orders, never a ghost', async () => {
    for (const pickupFirst of [true, false]) {
        __clearGameCache();
        const { gameId, A, B } = await throwInGame();
        const throwIn = moveOf(await mustReadTable(gameId), 0, 'attack', '6d');
        const sends = [() => runAction(gameId, B, PICKUP, V0), () => runAction(gameId, A, throwIn, V0)];
        if (!pickupFirst) sends.reverse();
        const [x, y] = await Promise.all(sends.map((s) => s()));
        const [pu, thrown] = pickupFirst ? [x, y] : [y, x];

        assert.equal(pu.status, ACTION_STATUS.APPLIED, 'the pickup always applies');
        const t = await mustReadTable(gameId);
        if (thrown.status === ACTION_STATUS.APPLIED) {
            // throw-in, then pickup: the defender took both cards.
            assert.equal(t.version, V0 + 2, 'two commits');
            assert.ok(hand(t, 1).includes('6d'), 'the thrown card went up with the pickup');
        } else {
            // pickup, then the throw-in against a closed round.
            assert.equal(thrown.status, ACTION_STATUS.REJECTED, 'the throw-in lost');
            assert.equal(thrown.rejectCode, REJECT_STALE_ROUND, 'to the round guard, not the rules');
            assert.equal(t.version, V0 + 1, 'one commit');
            assert.ok(hand(t, 0).includes('6d'), 'the throw-in card is still the attacker\'s');
        }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation (${pickupFirst ? 'pickup' : 'throw-in'} sent first): ${chk.detail}`);
    }
});

// ---------------------------------------------------------------------------
// This isolate's row cache, stale under another isolate's commit. The other
// isolate's commit is the real server path; afterwards the cache entry from
// before it is put back, which is exactly what an isolate that did not make the
// commit holds.
// ---------------------------------------------------------------------------

/** Runs `other` as if another isolate did it: this module's cache keeps the entry it had. */
async function inAnotherIsolate(gameId: string, other: () => Promise<unknown>): Promise<void> {
    const held = getCachedRow(gameId);
    assert.ok(held, 'fixture: this isolate holds the row warm');
    const saved = { ...held! };
    await other();
    noteCommittedRow(gameId, saved);
    assert.ok(getCachedRow(gameId)!.version < (await mustReadTable(gameId)).version, 'fixture: the cache is now stale');
}

/** What table_act says about a move on this isolate's CACHED row alone (the verdict the fresh re-check must overrule). */
function onCachedRow(gameId: string, actorId: string, m: PlayMove, intent: number): number {
    const row = getCachedRow(gameId)!;
    const bytes = (hex: string) => Buffer.from(hex.replace(/^\\x/, ''), 'hex');
    const t = fixtureTable();
    assert.equal(t.load(bytes(row.stateHex), bytes(row.rosterHex)), L.TABLE_OK);
    return t.act(actorId, m.wire, intent, row.roundEpoch);
}

test('stale cache: a rules rejection read off the cache is re-checked against the fresh row', async () => {
    const { gameId, A, B } = await throwInGame();
    // Warm this isolate's cache with A's throw-in (version V0 + 1).
    assert.equal((await runAction(gameId, A, moveOf(await mustReadTable(gameId), 0, 'attack', '6d'), V0)).status, ACTION_STATUS.APPLIED);
    const warm = hand(await mustReadTable(gameId), 0);
    // Another isolate commits B's pickup: the round closes and A refills.
    await inAnotherIsolate(gameId, () => runAction(gameId, B, PICKUP));
    const fresh = await mustReadTable(gameId);
    // An opening attack with a card A only drew in the refill: on the cached board
    // A does not hold it, so the cache alone would REJECT a legal move.
    const drawn = legalMoves(fresh, (_, s) => s === 0).find((m) => m.kind === 'attack' && m.cards.length === 1 && !warm.includes(cardText(m.cards[0])));
    assert.ok(drawn, 'fixture: A drew a card it can open with');
    assert.equal(onCachedRow(gameId, A, drawn!, fresh.roundEpoch), L.TABLE_REJECTED, 'fixture: the cached row alone rejects it');

    const out = await runAction(gameId, A, drawn!, fresh.roundEpoch);
    assert.equal(out.status, ACTION_STATUS.APPLIED, `the move is judged on the fresh row (got status ${out.status}, reject ${out.rejectCode})`);
    assert.equal(out.version, fresh.version + 1, 'and committed on top of it');
    const t = await mustReadTable(gameId);
    assert.deepEqual(t.battles.map((b) => cardText(b.attack)), [cardText(drawn!.cards[0])], 'the drawn card is on the table');
});

test('stale cache: an apply on a stale row loses the CAS, reloads, and keeps the other isolate\'s commit', async () => {
    const { gameId, A, B } = await throwInGame();
    assert.equal((await runAction(gameId, A, moveOf(await mustReadTable(gameId), 0, 'attack', '6d'), V0)).status, ACTION_STATUS.APPLIED);
    // Another isolate commits B's hand rearrangement (no events, a real commit).
    await inAnotherIsolate(gameId, () => runMeta(gameId, B, { type: 'rearrange-hand', card_indices: [5, 4, 3, 2, 1, 0] }));
    const fresh = await mustReadTable(gameId);
    assert.deepEqual(hand(fresh, 1), ['Qh', 'Jh', 'Th', '9h', '8h', '7h'], 'fixture: B\'s hand is reversed');

    // B covers from this isolate: legal on the cached row too, so it applies there
    // and its commit meets a newer version.
    const cover = moveOf(fresh, 1, 'cover', '7h');
    assert.equal(onCachedRow(gameId, B, cover, fresh.roundEpoch), L.TABLE_APPLIED, 'fixture: the cached row applies it too');
    const out = await runAction(gameId, B, cover, fresh.roundEpoch);
    assert.equal(out.status, ACTION_STATUS.APPLIED, 'the cover applies');
    assert.equal(out.version, fresh.version + 1, 'on top of the other isolate\'s commit');
    const t = await mustReadTable(gameId);
    assert.deepEqual(hand(t, 1), ['Qh', 'Jh', 'Th', '9h', '8h'], 'the reversed order survived: the retry reloaded, it did not overwrite');
    assert.equal(t.battles.find((b) => cardText(b.attack) === '6h')?.defense && cardText(t.battles.find((b) => cardText(b.attack) === '6h')!.defense!), '7h', 'and the cover is down');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `conservation: ${chk.detail}`);
});

test('stale cache: a move the cached round admits is stale-rejected once the fresh row shows the round closed', async () => {
    const { gameId, A, B } = await throwInGame();
    const t0 = await mustReadTable(gameId);
    assert.equal((await runAction(gameId, A, moveOf(t0, 0, 'attack', '6d'), V0)).status, ACTION_STATUS.APPLIED);
    const composed = await mustReadTable(gameId);   // B composes a cover here
    const cover = moveOf(composed, 1, 'cover', '7h');
    // Another isolate commits B's pickup (from B's other tab): the round closes.
    await inAnotherIsolate(gameId, () => runAction(gameId, B, PICKUP));
    const closed = await mustReadTable(gameId);
    assert.ok(closed.roundEpoch > composed.version, 'fixture: the round closed after the cover was composed');
    assert.equal(onCachedRow(gameId, B, cover, composed.version), L.TABLE_APPLIED, 'fixture: the cached row admits it');

    const out = await runAction(gameId, B, cover, composed.version);
    assert.equal(out.status, ACTION_STATUS.REJECTED, 'the cover does not apply');
    assert.equal(out.rejectCode, REJECT_STALE_ROUND, 'it is stale against the fresh row\'s round');
    const t = await mustReadTable(gameId);
    assert.equal(t.version, closed.version, 'nothing committed');
    assert.equal(hexOf(t.state), hexOf(closed.state), 'the closed round stands');
});

// ---------------------------------------------------------------------------
// Plan 5.2: two games' operations interleave in one module instance, with an
// await between one operation's load and its commit. The module's table is ONE
// resident game, so the first operation must not read it again after its
// kernel section: whatever it commits and broadcasts was copied out before the
// await. The await is a real one: the test holds game 1's row lock, so its
// commit_table blocks inside Postgres while game 2's whole operation runs.
// ---------------------------------------------------------------------------

async function untilALockWaits(): Promise<void> {
    for (let i = 0; i < 1000; i++) {
        const { rows } = await pgPool.query(
            'SELECT count(*)::int AS n FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid WHERE NOT l.granted AND a.datname = current_database()');
        if (rows[0].n > 0) return;
        await new Promise((r) => setTimeout(r, 5));
    }
    assert.fail('the first operation never reached its commit');
}

/** The state a move on a stored board commits, computed on the fixtures' own table. */
function expectedAfter(board: TableState, actorId: string, m: PlayMove): string {
    const t = fixtureTable();
    assert.equal(t.load(board.state, board.roster), L.TABLE_OK);
    assert.equal(t.act(actorId, m.wire, null, 0), L.TABLE_APPLIED, 'fixture: the move applies in memory');
    const p = t.commit(board.gameId, board.version + 1, 0);
    if (typeof p === 'number') throw new Error(`no products (${p})`);
    return hexOf(p.state);
}

for (const otherWriter of [false, true]) {
    test(`5.2: two games interleave across an await between load and commit${otherWriter ? ', and the first loses its CAS meanwhile' : ''}`, async () => {
        const g1 = await throwInGame('i1');
        const g2 = await throwInGame('i2');
        // Distinct boards, so a product of the wrong game cannot pass for the right one.
        await runMeta(g2.gameId, g2.A, { type: 'rearrange-hand', card_indices: [4, 3, 2, 1, 0] });
        __clearGameCache();
        const b1 = await mustReadTable(g1.gameId);
        const b2 = await mustReadTable(g2.gameId);
        const m1 = moveOf(b1, 0, 'attack', '6d');   // game 1: A throws in
        const m2 = moveOf(b2, 1, 'cover', '7h');    // game 2: B covers
        const logStart = broadcastLog.length;

        const lock = await pgPool.connect();
        let p1: ReturnType<typeof runAction> | null = null;
        let b1Other: TableState | null = null;
        try {
            await lock.query('BEGIN');
            await lock.query('SELECT id FROM games WHERE id = $1 FOR UPDATE', [g1.gameId]);
            p1 = runAction(g1.gameId, g1.A, m1, b1.version);
            p1.catch(() => { /* awaited below */ });
            await untilALockWaits();

            // Game 2's whole operation runs on the module's table while game 1's
            // commit waits.
            const r2 = await runAction(g2.gameId, g2.B, m2, b2.version);
            assert.equal(r2.status, ACTION_STATUS.APPLIED, 'game 2 applies');
            assert.equal(r2.version, b2.version + 1);

            if (otherWriter) {
                // Another writer commits game 1 first: B's hand reversed.
                const t = fixtureTable();
                assert.equal(t.load(b1.state, b1.roster), L.TABLE_OK);
                assert.equal(t.rearrangeHand(g1.B, [5, 4, 3, 2, 1, 0]), L.TABLE_OK);
                const p = t.commit(g1.gameId, b1.version + 1, 0);
                if (typeof p === 'number') throw new Error(`no products (${p})`);
                await lock.query('UPDATE games SET state = $2, version = version + 1 WHERE id = $1',
                    [g1.gameId, `\\x${hexOf(p.state)}`]);
                b1Other = { ...b1, state: p.state, version: b1.version + 1 };
            }
            await lock.query('COMMIT');
        } catch (e) {
            await lock.query('ROLLBACK');
            throw e;
        } finally {
            lock.release();
        }
        const r1 = await p1!;
        await settle();

        const base1 = b1Other ?? b1;
        assert.equal(r1.status, ACTION_STATUS.APPLIED, 'game 1 applies');
        assert.equal(r1.version, base1.version + 1, otherWriter ? 'after reloading past the other writer' : 'at its loaded version + 1');

        const s1 = await mustReadTable(g1.gameId);
        const s2 = await mustReadTable(g2.gameId);
        assert.equal(hexOf(s1.state), expectedAfter(base1, g1.A, m1), 'game 1 stores exactly its own move on its own board');
        assert.equal(hexOf(s2.state), expectedAfter(b2, g2.B, m2), 'game 2 stores exactly its own move on its own board');
        assert.deepEqual(s1.battles.map((b) => cardText(b.attack)), ['6h', '6d'], 'game 1: the throw-in is down');
        assert.deepEqual(s2.battles.map((b) => [cardText(b.attack), b.defense && cardText(b.defense)]), [['6h', '7h']], 'game 2: the cover is down');
        if (otherWriter) assert.deepEqual(hand(s1, 1), ['Qh', 'Jh', 'Th', '9h', '8h', '7h'], 'game 1 kept the other writer\'s commit');
        for (const g of [g1.gameId, g2.gameId]) {
            const chk = await checkCardConservation(g);
            assert.ok(chk.ok, `conservation on ${g}: ${chk.detail}`);
        }

        // Each game's pushes went to its own topics and carry its own move.
        for (const [g, s, actorSeat, kind] of [[g1, s1, 0, 'attack_pass'], [g2, s2, 1, 'cover']] as const) {
            const roster = { id: g.gameId, name: g.gameId, players: s.seats.map((x) => ({ player_id: x.id, name: x.name, is_ai: false })) };
            const mine = broadcastLog.slice(logStart).filter((e) => e.event === 'animation_events' && e.channel.includes(g.gameId));
            assert.equal(mine.length, 3, `${g.gameId}: one push per human and the spectator`);
            for (const e of mine) {
                const d = decodeEventWire(base64ToBytes(e.payload.b), roster, { preGood: [], prevGoodTs: null });
                assert.ok(d, `${g.gameId}: its push decodes against its roster`);
                assert.equal(e.payload.v, s.version, `${g.gameId}: the push carries its commit's version`);
                assert.ok(d!.events.some((ev) => ev.type === kind && ev.player_id === s.seats[actorSeat].id),
                    `${g.gameId}: the push carries its own ${kind}`);
            }
        }
    });
}
}
