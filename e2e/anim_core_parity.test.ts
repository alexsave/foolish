/* =============================================================================
 * Animation-core PARITY: the original TypeScript logic vs the C core, over the
 * same inputs. The owner's gate before any TS reference is deleted.
 * =============================================================================
 * For every policy that moved into c/src/anim_plan.h and now runs in production
 * through the wasm bridge, this drives BOTH:
 *   - the ORIGINAL TS (src/state/__ts_reference.ts, preserved verbatim), and
 *   - the C core (the runtime src/state/* wrappers, which delegate to wasm, and
 *     the animBuildPlan bridge directly),
 * and asserts identical outputs — over GENERATED scenarios (real evwire
 * sequences from seeded engine games, the way replay_steps_frames.test.ts drives
 * the kernel) PLUS the hostile/edge inputs the old TS tests pinned (malformed
 * keys, out-of-order versions, conflicting covers, dropped broadcasts).
 *
 * Pure kernel/wasm test — needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { start_game } from '../server/api/common/game_lifecycle.ts';
import { game_done } from '../server/api/common/common_utils.ts';
import {
    Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey,
} from '../server/api/core/types.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import {
    kernelReplayEncodeV6FromGame, replayEventFrames, replayStepCount,
    animBuildPlan, animEventTypeCode, ANIM_LOC,
} from '../sdk/ts/wasm/bots.ts';
import { decodeEventWire, DecodedSequence } from '../sdk/ts/wire/evwire.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import type { ViewRoster } from '../sdk/ts/wire/view.ts';
import { createCardEventString, getCardKey } from '../src/utils/animationUtils.ts';

// The RUNTIME (C-backed) implementations.
import { staleOptimisticKeysOnTable } from '../src/state/optimisticAnimation.ts';
import { resolveUnconfirmedAttackCovers } from '../src/state/optimisticConflicts.ts';
import { shouldDropStaleSequence } from '../src/state/clientReconcile.ts';
// The ORIGINAL TS reference implementations, kept for exactly this comparison.
import {
    staleOptimisticKeysOnTableTsReference,
    resolveUnconfirmedAttackCoversTsReference,
    shouldDropStaleSequenceTsReference,
    buildAnimPlanTsReference, RefPlanEvent,
} from '../src/state/__ts_reference.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {};
}

type Card = { suit: number; value: number };

const hexToBytes = (h: string) => new Uint8Array((h.match(/.{2}/g) ?? []).map(b => parseInt(b, 16)));
const SEEDS = [
    '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff',
    'ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100',
    '0123456789abcdef0123456789abcdeffedcba9876543210fedcba9876543210',
    'a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90',
];

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy,
});
function mkGame(np: number, seedHex: string): Game {
    return {
        players: Array.from({ length: np }, (_, i) => mkPlayer(i, 'handwritten' as StrategyKey)),
        deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [], game_seed: seedHex,
    } as unknown as Game;
}
async function playSeeded(np: number, seedHex: string): Promise<Game | null> {
    const game = mkGame(np, seedHex);
    __setDealSeedOverride(hexToBytes(seedHex));
    try {
        start_game(game);
        for (let guard = 0; guard < 20000 && game_done(game) === null; guard++) {
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return null;
        }
    } finally {
        __setDealSeedOverride(null);
    }
    return game_done(game) !== null ? game : null;
}
const roster = (game: Game): ViewRoster => ({
    id: game.id, name: game.name,
    players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key })),
});

// Collect every per-step decoded evwire sequence of a seeded game (spectator).
async function collectFrames(np: number, seedHex: string): Promise<{ game: Game; seqs: DecodedSequence[] } | null> {
    const game = await playSeeded(np, seedHex);
    if (!game) return null;
    const code = kernelReplayEncodeV6FromGame(game, hexToBytes(seedHex), undefined, 1 << 20);
    const steps = replayStepCount(code);
    if (steps <= 0) return null;
    const frames = replayEventFrames(code, -1);
    const ctx = { preGood: [], prevGoodTs: null, now: () => 0 };
    const seqs: DecodedSequence[] = [];
    for (const frame of frames) {
        const seq = decodeEventWire(frame, roster(game), ctx);
        if (seq) seqs.push(seq);
    }
    return { game, seqs };
}

const tableCardsOf = (gs: any): Card[] => (gs?.table_battles ?? [])
    .flatMap((b: any) => (b.defense ? [b.attack, b.defense] : [b.attack]));
const locCode = (loc: string | undefined): number => (loc && loc in ANIM_LOC) ? ANIM_LOC[loc] : 0xff;

// Convert a decoded sequence + its committed board into animBuildPlan inputs.
function planInputs(seq: DecodedSequence, game: Game) {
    const np = game.players.length;
    const seatOf = new Map(game.players.map((p, i) => [p.player_id, i]));
    const finalG: any = seq.game;
    const events: RefPlanEvent[] = seq.events.map((e) => {
        const cards = (e.cards ?? []) as Card[];
        const mask = cards.length > 0 && cards.every((c) => c.suit < 0);
        return {
            type: animEventTypeCode(e.type),
            seat: e.player_id !== undefined ? (seatOf.get(e.player_id) ?? null) : null,
            from: locCode(e.from_location),
            to: locCode(e.to_location),
            mask,
            cards,
        };
    });
    const finalHand = finalG.players.map((p: any) => p.hand_length ?? (p.hand?.length ?? 0));
    return { events, np, finalDeck: finalG.deck_length ?? 0, finalDiscard: finalG.discard_pile_length ?? 0, finalHand };
}

// ============================ 1. PLAN PARITY ==============================
test('plan building: C animBuildPlan == TS reference over real evwire sequences', async () => {
    let frames = 0, stepsSeen = 0;
    for (let np = 2; np <= 4; np++) {
        for (const seed of SEEDS) {
            const got = await collectFrames(np, seed);
            if (!got) continue;
            for (const seq of got.seqs) {
                const { events, finalDeck, finalDiscard, finalHand } = planInputs(seq, got.game);
                if (events.length === 0 || events.length > 64) continue;
                const cPlan = animBuildPlan(events, np, finalDeck, finalDiscard, finalHand);
                const tPlan = buildAnimPlanTsReference(events, np, finalDeck, finalDiscard, finalHand);
                assert.deepEqual(cPlan, tPlan, `np=${np} seed=${seed.slice(0, 8)} plan diverged`);
                frames++; stepsSeen += cPlan.nSteps;
            }
        }
    }
    assert.ok(frames > 20, `expected many frames, got ${frames}`);
    assert.ok(stepsSeen > 0, 'plans carried steps');
});

// Bonus: the plan's derived per-step counts match the KERNEL's real snapshots,
// so the count-freeze is not just self-consistent with the TS mirror but true to
// the board the engine produced (discard + per-seat hand are unambiguous).
test('plan building: derived counts match the kernel per-step snapshots', async () => {
    let checks = 0;
    for (let np = 2; np <= 4; np++) {
        const got = await collectFrames(np, SEEDS[0]);
        if (!got) continue;
        for (const seq of got.seqs) {
            const { events, np: n, finalDeck, finalDiscard, finalHand } = planInputs(seq, got.game);
            if (events.length === 0 || events.length > 64) continue;
            const plan = animBuildPlan(events, n, finalDeck, finalDiscard, finalHand);
            for (let i = 0; i < seq.events.length; i++) {
                const gs: any = seq.events[i].game_state;
                if (!gs) continue;
                assert.equal(plan.steps[i].discard, gs.discard_pile_length,
                    `discard count mismatch at step ${i}`);
                for (let s = 0; s < n; s++) {
                    const hl = gs.players?.[s]?.hand_length ?? gs.players?.[s]?.hand?.length ?? 0;
                    assert.equal(plan.steps[i].hand[s], hl, `hand[${s}] mismatch at step ${i}`);
                }
                checks++;
            }
        }
    }
    assert.ok(checks > 0, 'expected snapshot cross-checks');
});

// ======================== 2. STALE-OPTIMISTIC PARITY ======================
test('staleOptimisticKeysOnTable: C == TS reference (generated + edge cases)', async () => {
    const runOne = (keys: string[], table: Card[], events: any[]) => {
        const c = staleOptimisticKeysOnTable(keys, table, events);
        const t = staleOptimisticKeysOnTableTsReference(keys, table, events);
        assert.deepEqual(c, t, 'stale-optimistic diverged');
    };

    // Edge cases the old TS test pinned.
    const card: Card = { suit: 1, value: 9 };
    const optKey = createCardEventString('attack_pass', card, 'hand', 'table', 'p0');
    runOne([optKey], [card], [{ type: 'attack_pass', cards: [card] }]);          // named -> keep
    runOne([optKey], [card], [{ type: 'cover', cards: [{ suit: 2, value: 10 }] }]); // dropped-broadcast -> release
    runOne([optKey], [], []);                                                     // not on table -> keep
    runOne(['not-json', '{}', '{"card":null}', optKey], [card], []);              // malformed keys + on-table
    runOne([optKey, createCardEventString('cover', { suit: 3, value: 7 }, 'hand', 'table', 'p0')],
           [card, { suit: 3, value: 7 }], [{ type: 'attack_pass', cards: [card] }]);

    // Generated: from real game frames, key every table card + inject a couple of
    // "dropped broadcast" and "named" mixes.
    let scenarios = 0;
    for (let np = 2; np <= 4; np++) {
        const got = await collectFrames(np, SEEDS[1]);
        if (!got) continue;
        for (const seq of got.seqs) {
            const table = tableCardsOf(seq.game).filter((c) => c.suit >= 0);
            if (table.length === 0) continue;
            const keys = table.map((c) => createCardEventString('attack_pass', c, 'hand', 'table', 'p0'));
            // events name HALF the table (the other half is a "dropped broadcast").
            const named = table.slice(0, Math.ceil(table.length / 2));
            runOne(keys, table, [{ type: 'attack_pass', cards: named }]);
            runOne(keys, table, []);                     // nothing named -> all released
            runOne(keys, [], [{ type: 'attack_pass', cards: named }]); // nothing on table -> keep all
            scenarios += 3;
        }
    }
    assert.ok(scenarios > 10, `expected generated scenarios, got ${scenarios}`);
});

// ======================= 3. RESOLVE (revert) PARITY =======================
test('resolveUnconfirmedAttackCovers: C == TS reference (generated + edge cases)', async () => {
    const runOne = (pending: Card[], server: Card[], events: any[], fin: any, coverKeys?: Set<string>) => {
        const c = resolveUnconfirmedAttackCovers(pending, server, events, fin, coverKeys);
        const t = resolveUnconfirmedAttackCoversTsReference(pending, server, events, fin, coverKeys);
        assert.deepEqual(c, t, 'resolve diverged');
    };

    // Edge cases (the two scenarios + capacity + cover-exclusion + accepted).
    const my: Card = { suit: 0, value: 7 };
    runOne([my], [], [{ type: 'pickup', cards: [my] }], { defender: 1, players: [{}, { hand_length: 7 }], table_battles: [] }); // SCENARIO B
    runOne([{ suit: 3, value: 11 }], [{ suit: 3, value: 6 }], [{ type: 'attack_pass', cards: [{ suit: 3, value: 6 }] }],
           { defender: 0, players: [{ hand_length: 6 }], table_battles: [{ attack: { suit: 3, value: 6 }, defense: null }] }); // SCENARIO A
    runOne([{ suit: 0, value: 5 }, { suit: 1, value: 5 }], [], [{ type: 'attack_pass', cards: [] }],
           { defender: 1, players: [{}, { hand_length: 5 }], table_battles: Array.from({ length: 5 }, () => ({ defense: null })) }); // capacity revert
    {
        const attack: Card = { suit: 0, value: 5 }, cover: Card = { suit: 2, value: 9 };
        const coverKeys = new Set([getCardKey(cover)]);
        runOne([attack, cover], [], [{ type: 'attack_pass', cards: [] }],
               { defender: 1, players: [{}, { hand_length: 5 }], table_battles: Array.from({ length: 5 }, () => ({ defense: null })) }, coverKeys); // cover-excl
    }
    runOne([{ suit: 1, value: 8 }], [{ suit: 1, value: 8 }], [{ type: 'attack_pass', cards: [] }],
           { defender: 1, players: [{}, { hand_length: 6 }], table_battles: [] }); // accepted -> empty

    // Generated from real frames: use the frame's own table + events + board.
    let scenarios = 0;
    for (let np = 2; np <= 4; np++) {
        const got = await collectFrames(np, SEEDS[2]);
        if (!got) continue;
        for (const seq of got.seqs) {
            const finalG: any = seq.game;
            const server = tableCardsOf(finalG).filter((c) => c.suit >= 0);
            // Pending: a couple of plausible cards not on the server table.
            const pending: Card[] = [{ suit: 0, value: 6 }, { suit: 2, value: 12 }].filter(
                (pc) => !server.some((s) => s.suit === pc.suit && s.value === pc.value));
            if (pending.length === 0) continue;
            const coverKeys = new Set([getCardKey(pending[pending.length - 1])]); // mark last as a cover
            runOne(pending, server, seq.events, finalG, coverKeys);
            runOne(pending, server, seq.events, finalG);            // no cover keys
            runOne(pending, [], seq.events, finalG, coverKeys);     // nothing on server table
            scenarios += 3;
        }
    }
    assert.ok(scenarios > 10, `expected generated scenarios, got ${scenarios}`);
});

// ========================= 4. VERSION-GATE PARITY =========================
test('shouldDropStaleSequence: C == TS reference (out-of-order + null versions)', () => {
    const cases: [number | null, number | null][] = [
        [null, null], [null, 3], [5, null], [5, 5], [5, 4], [5, 6],
        [0, 0], [10, 9], [10, 11], [1, 1000], [1000, 1],
    ];
    for (const [last, incoming] of cases) {
        assert.equal(
            shouldDropStaleSequence(last, incoming),
            shouldDropStaleSequenceTsReference(last, incoming),
            `gate diverged for (${last}, ${incoming})`);
    }
});
