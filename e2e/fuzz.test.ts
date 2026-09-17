// Adversarial / illegal-input fuzzer. Fires malformed and rule-breaking action
// requests - the bytes a malicious or buggy client could POST - through the REAL
// server move path (packed_action.ts executePackedAction: the kernel's request
// decode, the table_io CAS loop, table_act for the auth id's seat, commit_table)
// against a real Postgres, and asserts the hard safety invariant after EVERY
// attempt:
//
//   card conservation holds - no input ever duplicates or loses a card.
//
// Plus: an input that can never be legal is never applied, every refusal is a
// clean one (a kernel verdict or a named refusal, never a low-level throw), and
// targeted checks that obviously-illegal inputs are rejected for the right reason.
//
// Migrated to the C Table (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b). What changed
// and where the old cases went:
//   - The generator is table-shaped and local (see `generators` below): the shared
//     e2e/helpers/fuzz_moves.ts draws from a TypeScript Game and still serves
//     e2e/table_parity.test.ts. The hostile inputs are the same families, as the
//     bytes a client can actually send.
//   - The JSON-shape families (cards as a string / object / number, card fields as
//     strings or nested junk, null fields) have no byte form: a move is packed
//     only. Their byte-level counterparts are here (unknown kinds, truncated and
//     over-long wires, garbage card bytes, malformed request envelopes), and a
//     JSON move naming any player is refused by e2e/table_server_seat.test.ts
//     ('action: a JSON body naming another player acts for nobody').
//   - registerAttackValidation (run by e2e/validation/handlers_validation.test.ts)
//     held the TS handleAttack twin; it now holds the C Table's table_act on a
//     kernel-built board, in memory, with the kernel's reject reason.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { AWIRE_KIND, ACTION_STATUS, decodeActionResponse, encodeActionRequest, wireCard } from '../sdk/ts/wire/awire.ts';
import { executePackedAction, MalformedActionRequest } from '../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { GameNotFound, TableRefusal, __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { fixture, fixtureTable } from './helpers/table_fixture.ts';
import { checkCardConservation, legalMoves, mustReadTable, residentBoard, type PlayCard, type TableState } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// Deterministic: moves, hostile inputs and every deal come from the suite seed,
// so a found exploit reproduces from the printed seed (E2E_SEED_FUZZ).
const rng = suiteRng('fuzz');
const { int: ri, pick } = rng;

const wc = (c: PlayCard) => wireCard(c);
const bytes = (...xs: number[]) => Uint8Array.from(xs.map((x) => x & 0xff));

// ---- handpicked, pure validation (no DB): the always-reject invariants -------
export function registerAttackValidation(): void {
    test('attack: forged card, identical-duplicate, and non-member attacks are all rejected', () => {
        // The kernel builds the board: atk (seat 0) attacks def (seat 1).
        const fx = fixture().seats([{ id: 'atk', name: 'atk' }, { id: 'def', name: 'def' }])
            .status(L.GAME_STATUS_PLAYING).attacker(0).defender(1).powerSuit(0)
            .hand(0, '6c 6d 8h').hand(1, 'Ks').deck('').discard(32).build();
        const table = fixtureTable();
        const hand0 = (): PlayCard[] => { table.load(fx.state, fx.roster); return residentBoard('g', fx.state, fx.roster).seats[0].hand; };
        const original = hand0();
        assert.equal(original.length, 3, 'fixture: the attacker holds three cards');

        const attack = (actor: string, cards: PlayCard[]) => {
            assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK);
            const rc = table.act(actor, Uint8Array.from([AWIRE_KIND.attack, cards.length, ...cards.map(wc)]), null, 0);
            return { rc, reject: rc === L.TABLE_REJECTED ? table.reject() : 0, hand: residentBoard('g', fx.state, fx.roster).seats[0].hand };
        };

        // forged: a card the attacker does not hold
        const forged = attack('atk', [{ suit: 3, value: 9 }]);
        assert.equal(forged.rc, L.TABLE_REJECTED, 'forged card');
        assert.equal(forged.reject, L.ENGINE_REJECT_NOT_IN_HAND, 'forged card: not in hand');
        assert.deepEqual(forged.hand, original, 'forged card: the hand is untouched');
        // the object-identity duplicate hole: [X, X] must be rejected, never duplicated
        const dup = attack('atk', [original[0], original[0]]);
        assert.equal(dup.rc, L.TABLE_REJECTED, 'identical duplicate');
        assert.equal(dup.reject, L.ENGINE_REJECT_DUPLICATES, 'identical duplicate: duplicates');
        assert.deepEqual(dup.hand, original, 'identical duplicate: the hand is untouched');
        // a player who isn't in the game
        assert.equal(attack('ghost', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'non-member');
        assert.equal(attack('', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'empty actor id');
        assert.equal(attack('at', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'a prefix of a seated id');
    });
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });

async function freshGame(): Promise<string> {
    const gameId = `f${uuid().slice(0, 6)}`;
    const h0 = uuid();
    await seedLobby(gameId, [
        { id: h0, name: 'H0', ready: false },
        { id: uuid(), name: 'H1' },
        { id: uuid(), name: 'B0', brain: 'random' },
    ]);
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, () => ri(256)));
    await runMeta(gameId, h0, { type: 'start' });
    return gameId;
}

// ---- the table-shaped adversarial generator ------------------------------------

interface Hostile {
    family: string;
    /** The auth user id the request is sent as. */
    actor: string;
    /** The whole request body. */
    body: Uint8Array;
    /** No state makes this input legal: it must never be applied. */
    neverLegal: boolean;
}

const garbageByte = () => pick([52, 60, 99, 127, 200, 0xfe, 0xff]);
const anyWire = () => bytes(ri(256), ri(256), ...Array.from({ length: ri(6) }, () => ri(256)));

function generators(): ((t: TableState) => Hostile)[] {
    let cur: TableState;
    const seatId = (i: number) => cur.seats[i]?.id ?? uuid();
    const req = (wire: Uint8Array, intent?: number) => encodeActionRequest(cur.gameId, wire, intent);
    const someHandCard = (): PlayCard | null => {
        const withHands = cur.seats.filter((s) => s.hand.length);
        return withHands.length ? pick(pick(withHands).hand) : null;
    };
    const aCard = (): number => { const c = someHandCard(); return c ? wc(c) : ri(52); };
    const attackerId = () => {
        const atks = cur.seats.map((s, i) => ({ s, i })).filter(({ s, i }) => i !== cur.defender && s.status === L.PLAYER_STATUS_IN);
        return (atks.length ? pick(atks).s : cur.seats[0]).id;
    };
    const firstAttacker = () => seatId(cur.firstAttacker);
    const defender = () => seatId(cur.defender);
    const h = (f: string, actor: string, body: Uint8Array, neverLegal: boolean): Hostile => ({ family: f, actor, body, neverLegal });
    const gens: ((t: TableState) => Hostile)[] = [
        // 1) DUPLICATE identical card in one attack - the object-identity dedup hole.
        () => { const c = aCard(); return h('dup-attack', firstAttacker(), req(bytes(AWIRE_KIND.attack, 2, c, c)), true); },
        // 2) duplicate identical cover card
        () => { const c = aCard(); const a = cur.battles[0] ? wc(cur.battles[0].attack) : ri(52); return h('dup-cover', defender(), req(bytes(AWIRE_KIND.cover, 2, c, c, a, a)), true); },
        // 3) duplicate identical pass card
        () => { const c = aCard(); return h('dup-pass', defender(), req(bytes(AWIRE_KIND.pass, 2, c, c)), true); },
        // 4) forged card, possibly not in hand
        () => h('forged', attackerId(), req(bytes(AWIRE_KIND.attack, 1, ri(52))), false),
        // 5) out-of-range garbage card byte (hidden, none, past the deck)
        () => h('garbage-card', attackerId(), req(bytes(AWIRE_KIND.attack, 1, garbageByte())), false),
        // 6) wrong role: defender attacks with a card of their own
        () => { const hand = cur.seats[cur.defender]?.hand ?? []; const c = hand.length ? wc(pick(hand)) : ri(52); return h('defender-attacks', defender(), req(bytes(AWIRE_KIND.attack, 1, c)), true); },
        // 7) wrong role: an attacker tries to cover / pick up
        () => pick([
            h('attacker-covers', attackerId(), req(bytes(AWIRE_KIND.cover, 1, aCard(), cur.battles[0] ? wc(cur.battles[0].attack) : ri(52))), true),
            h('attacker-picks-up', attackerId(), req(bytes(AWIRE_KIND.pickup, 0)), true),
        ]),
        // 8) a player who is not in the game
        () => h('not-seated', uuid(), req(pick([bytes(AWIRE_KIND.attack, 1, aCard()), bytes(AWIRE_KIND.pickup, 0), bytes(AWIRE_KIND.good, 0), bytes(AWIRE_KIND.pass, 1, aCard())])), true),
        // 9) empty / over-long / count-past-the-body payloads
        () => { const c = aCard(); return pick([
            h('attack-empty', attackerId(), req(bytes(AWIRE_KIND.attack, 0)), true),
            h('attack-20-same', attackerId(), req(bytes(AWIRE_KIND.attack, 20, ...Array(20).fill(c))), true),
            h('attack-29-cards', attackerId(), req(bytes(AWIRE_KIND.attack, 29, ...Array.from({ length: 29 }, (_, i) => i))), true),
            h('attack-count-past-body', attackerId(), req(bytes(AWIRE_KIND.attack, 255, c, c)), true),
        ]); },
        // 10) mixed-value first attack
        () => { const hand = cur.seats[cur.firstAttacker]?.hand ?? []; const cs = hand.length >= 2 ? [wc(hand[0]), wc(hand[1])] : [ri(52), ri(52)]; return h('mixed-first-attack', firstAttacker(), req(bytes(AWIRE_KIND.attack, 2, ...cs)), false); },
        // 11) cover with a non-covering / off-table attack card
        () => h('cover-off-table', defender(), req(bytes(AWIRE_KIND.cover, 1, aCard(), garbageByte())), false),
        // 12) good by the defender / out of turn
        () => h('defender-good', defender(), req(bytes(AWIRE_KIND.good, 0)), true),
        // 13) mismatched cover/attack lengths
        () => h('cover-mismatched', defender(), req(pick([bytes(AWIRE_KIND.cover, 1, aCard()), bytes(AWIRE_KIND.cover, 2, aCard(), aCard(), aCard())])), true),
        // 14) malformed wire: unknown kind, one byte, cards on a pickup/good, extra trailing bytes
        () => pick([
            h('unknown-kind', attackerId(), req(bytes(5 + ri(251), 0)), true),
            h('one-byte-wire', attackerId(), encodeActionRequest(cur.gameId, bytes(AWIRE_KIND.pickup)), true),
            h('pickup-with-cards', defender(), req(bytes(AWIRE_KIND.pickup, 1, aCard())), true),
            h('good-with-cards', attackerId(), req(bytes(AWIRE_KIND.good, 1, aCard())), true),
            h('trailing-bytes', attackerId(), req(bytes(AWIRE_KIND.attack, 1, aCard(), 0)), true),
        ]),
        // 15) random bytes as the wire
        () => h('random-wire', pick(cur.seats).id, req(anyWire()), false),
        // 16) injection-ish strings as the actor (parameterized queries must shrug; the kernel seats nobody)
        () => h('injection-actor', pick(["1' OR '1'='1", "'; DELETE FROM player_hands; --", '../../etc/passwd', '__proto__', 'constructor']),
            req(bytes(AWIRE_KIND.attack, 1, aCard())), true),
        // 17) bounded-large payloads (DoS attempt - must stay bounded, not hang/OOM)
        () => pick([
            h('attack-300-cards', firstAttacker(), req(Uint8Array.from([AWIRE_KIND.attack, 300 & 0xff, ...Array.from({ length: 300 }, () => aCard())])), true),
            h('body-8kb', firstAttacker(), req(Uint8Array.from({ length: 8192 }, () => ri(256))), true),
            // past the kernel's IO buffer: the host must refuse it, not overflow a copy
            h('body-past-io-cap', firstAttacker(), req(new Uint8Array(256 * 1024).fill(AWIRE_KIND.attack)), true),
        ]),
        // 18) an empty actor id
        () => h('empty-actor', '', req(pick([bytes(AWIRE_KIND.attack, 1, aCard()), bytes(AWIRE_KIND.pickup, 0)])), true),
        // 19) malformed request envelopes
        () => pick([
            h('envelope-bad-format', attackerId(), Uint8Array.of(3 + ri(250), 1, 0x61, AWIRE_KIND.pickup, 0), true),
            h('envelope-empty', attackerId(), new Uint8Array(0), true),
            h('envelope-gid-past-body', attackerId(), Uint8Array.of(1, 200, 0x61, 0x62), true),
            h('envelope-v2-short', attackerId(), Uint8Array.of(2, 0, 1, 2, 3), true),
            h('envelope-other-game', attackerId(), encodeActionRequest(pick(["g'; DROP TABLE games;--", 'nope', ' ', 'x'.repeat(255)]), bytes(AWIRE_KIND.pickup, 0)), true),
            h('envelope-invalid-utf8-gid', attackerId(), Uint8Array.of(1, 2, 0xff, 0xfe, AWIRE_KIND.pickup, 0), true),
        ]),
        // 20) intent versions at the edges on a real move (stale, zero, far future)
        () => {
            const moves = legalMoves(cur);
            if (!moves.length) return h('intent-edge', attackerId(), req(bytes(AWIRE_KIND.pickup, 0), 0), false);
            const m = pick(moves);
            return h('intent-edge', m.playerId, req(m.wire, pick([0, cur.roundEpoch - 1, 0xffffffff])), false);
        },
    ];
    return gens.map((g) => (t: TableState) => { cur = t; return g(t); });
}

type Outcome = 'applied' | 'rejected' | 'moot' | 'malformed' | 'refused' | 'not-found' | 'crash';

// The move path as the endpoint runs it. Anything but a kernel verdict or a named
// refusal is crash-class: it would reach the client as an unexplained error.
async function send(x: Hostile): Promise<{ outcome: Outcome; detail: string }> {
    try {
        const out = await executePackedAction(x.body, x.actor, 'fuzz');
        const r = decodeActionResponse(out.body);
        if (!r) return { outcome: 'crash', detail: 'the response did not decode' };
        const outcome = r.status === ACTION_STATUS.APPLIED ? 'applied' : r.status === ACTION_STATUS.MOOT ? 'moot' : 'rejected';
        return { outcome, detail: `code ${r.rejectCode}` };
    } catch (e) {
        if (e instanceof MalformedActionRequest) return { outcome: 'malformed', detail: e.message };
        if (e instanceof TableRefusal) return { outcome: 'refused', detail: e.message };
        if (e instanceof GameNotFound) return { outcome: 'not-found', detail: e.message };
        return { outcome: 'crash', detail: `${(e as Error)?.name}: ${(e as Error)?.message ?? e}` };
    }
}

test('adversarial fuzz: no illegal/malformed input ever duplicates or loses a card', async () => {
    const ITER = Number(process.env.FUZZ_ITERS || 3000);
    const gens = generators();
    const violations: string[] = [];
    const appliedIllegal: string[] = [];
    const crashes: string[] = [];
    const tally = new Map<Outcome, number>();
    const byFamily = new Map<string, Set<Outcome>>();
    let gameId = await freshGame();
    let attempts = 0;

    for (let i = 0; i < ITER; i++) {
        let t = await mustReadTable(gameId);
        if (t.status !== L.GAME_STATUS_PLAYING) { gameId = await freshGame(); t = await mustReadTable(gameId); }

        // 35% legal move to keep the game evolving through phases; else adversarial.
        if (rng.next() < 0.35) {
            const moves = legalMoves(t);
            if (moves.length) { const m = pick(moves); try { await runAction(gameId, m.playerId, m); } catch { /* */ } }
            continue;
        }

        attempts++;
        const x = pick(gens)(t);
        const { outcome, detail } = await send(x);
        tally.set(outcome, (tally.get(outcome) ?? 0) + 1);
        (byFamily.get(x.family) ?? byFamily.set(x.family, new Set()).get(x.family)!).add(outcome);
        const where = `seed=${rng.seed} iter=${i} family=${x.family} actor=${JSON.stringify(x.actor)} body=${Buffer.from(x.body.slice(0, 48)).toString('hex')}`;
        if (outcome === 'crash' && crashes.length < 5) crashes.push(`${where} -> ${detail}`);
        if (outcome === 'applied' && x.neverLegal && appliedIllegal.length < 5) appliedIllegal.push(where);

        const chk = await checkCardConservation(gameId);
        if (!chk.ok) violations.push(`${where} -> ${chk.detail}`);
    }

    const summary = [...tally.entries()].map(([k, v]) => `${k}=${v}`).join(' ');
    // Hard invariant: no adversarial input may ever duplicate or lose a card.
    assert.equal(violations.length, 0, `card conservation broken by adversarial input:\n  ${violations.slice(0, 3).join('\n  ')}`);
    const refusedClean = (tally.get('rejected') ?? 0) + (tally.get('malformed') ?? 0) + (tally.get('refused') ?? 0);
    assert.ok(attempts > 100 && refusedClean > 0, `fuzz ran (attempts=${attempts} ${summary})`);
    // An input no state makes legal is never applied.
    assert.equal(appliedIllegal.length, 0, `a never-legal input was APPLIED:\n  ${appliedIllegal.join('\n  ')}`);
    // Malformed payloads produce clean refusals, never low-level throws.
    assert.equal(crashes.length, 0, `malformed input produced ungraceful crash-class error(s):\n  ${crashes.join('\n  ')}`);
    // The process surviving ITER hostile requests IS the no-crash assertion.
    process.stdout.write(`[fuzz] attempts=${attempts} ${summary}\n`);
    if (process.env.E2E_VERBOSE) for (const [f, o] of [...byFamily].sort()) process.stdout.write(`[fuzz]   ${f}: ${[...o].join(',')}\n`);
});

test('targeted: forged cards and non-members are always rejected', async () => {
    const gameId = await freshGame();
    const t = await mustReadTable(gameId);
    const attacker = t.seats[t.firstAttacker];
    // A forged card = a real card the attacker does not hold. (An out-of-range
    // byte is WRONG here: the kernel clamps it onto a real card, and on deals
    // where the attacker holds that card the "forged" attack is legitimately
    // legal - a 1-in-6 flake.)
    let forged: PlayCard | null = null;
    for (let s = 0; s < 4 && !forged; s++)
        for (let v = 5; v <= 13 && !forged; v++)
            if (!attacker.hand.some((c) => c.suit === s && c.value === v)) forged = { suit: s, value: v };
    const res = await runAction(gameId, attacker.id, bytes(AWIRE_KIND.attack, 1, wc(forged!)));
    assert.equal(res.status, ACTION_STATUS.REJECTED, 'forged card rejected');
    assert.equal(res.rejectCode, L.ENGINE_REJECT_NOT_IN_HAND, 'forged card: not in hand');
    assert.equal(res.version, t.version, 'the response carries the unchanged version');
    // a player who isn't in the game, sending the attacker's own card
    await assert.rejects(runAction(gameId, uuid(), bytes(AWIRE_KIND.attack, 1, wc(attacker.hand[0]))), /not in game/i, 'non-member rejected');
    assert.equal((await mustReadTable(gameId)).version, t.version, 'nothing committed');
});

test('regression: sending the same card twice in one move is rejected (no duplication)', async () => {
    const gameId = await freshGame();
    const t = await mustReadTable(gameId);
    const fa = t.seats[t.firstAttacker];
    const x = wc(fa.hand[0]);
    // attack with [X, X] - the object-identity dedup hole - must be rejected.
    const res = await runAction(gameId, fa.id, bytes(AWIRE_KIND.attack, 2, x, x));
    assert.equal(res.status, ACTION_STATUS.REJECTED, 'duplicate attack rejected');
    assert.equal(res.rejectCode, L.ENGINE_REJECT_DUPLICATES, 'duplicate attack: duplicates');
    // and the durable state is untouched (the refusal committed nothing).
    const later = await mustReadTable(gameId);
    assert.equal(later.version, t.version, 'nothing committed');
    assert.deepEqual(later.seats[t.firstAttacker].hand, fa.hand, 'the hand is untouched');
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `state intact after rejected duplicate: ${chk.detail}`);
});

registerAttackValidation();
}
