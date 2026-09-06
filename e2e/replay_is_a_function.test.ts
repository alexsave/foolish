// Two runs of one game are the same run.
//
// The product's whole determinism story is that a game is a pure function of
// its deal seed: 32 crypto bytes drawn once, saved to games.game_seed, and
// everything after them derived. That story was quietly false in the shape
// nobody looks at - not the cards, which were always right, but the rows the
// game writes down. Every session log got `id: crypto.randomUUID()` and
// `created_at: new Date()`, so replaying one game twice produced two states
// that no equality check could match, and "did this change anything?" had no
// answer you could compute.
//
// So this plays the SAME deal twice, from the same pinned seed and the same
// pinned clock, and asserts the two games are identical all the way down -
// hands, table, deck, and the log rows. It is the assertion the fix exists for:
// swap derivedUuid back to crypto.randomUUID in sdk/ts/wasm/engine.ts, or drop
// the clock hook, and this goes red on the first log row.
//
// It needs no database. The kernel and the rules surface are the whole subject.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { __setDealSeedOverride, __setEngineClock } from '../sdk/ts/wasm/engine.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { executeBotMove } from '../server/api/common/pure_bot_actions.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../server/api/core/types.ts';
import { suiteRng } from './helpers/rng.ts';

const rng = suiteRng('replay_is_a_function');

// A pinned deal. Derived from the suite seed rather than typed in, so widening
// the search is `E2E_SEED_REPLAY_IS_A_FUNCTION=<n>` and not an edit.
function dealSeed(): Uint8Array {
    const r = rng.fork('deal');
    return Uint8Array.from({ length: 32 }, () => r.int(256));
}

// A clock that does not move. The stamps a game writes are real product data -
// the replay extras read per-move timing off them - so the engine still calls a
// clock; what it must also do is let a caller say which one.
const FIXED_MS = 1_700_000_000_000;

function freshGame(id: string, players: number): Game {
    return {
        id,
        name: id,
        status: GAME_STATUS.WAITING,
        players: Array.from({ length: players }, (_, i) => ({
            player_id: `p${i}`,
            name: `P${i}`,
            status: PLAYER_STATUS.READY,
            is_ai: true,
            hand: [],
            hand_length: 0,
            awaiting_attack: false,
            strategy_key: STRATEGY_KEY.RANDOM,
        })),
        deck: [],
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
        logs: [],
        belief_logs: [],
        game_seed: null,
    } as unknown as Game;
}

/** Deal and play `steps` moves, always taking the same legal move. */
function playOnce(id: string, players: number, steps: number): Game {
    const g = freshGame(id, players);
    start_game(g);
    for (let i = 0; i < steps && g.status === GAME_STATUS.PLAYING; i++) {
        // The FIRST legal move of the FIRST seat that has one, every time: the
        // subject here is reproducibility, so the move choice must not be the
        // thing under test.
        let played = false;
        for (const p of g.players) {
            if (p.status !== PLAYER_STATUS.IN) continue;
            const move = calculateLegalMoves(g, p.player_id).find((m) => m.type !== 'wait');
            if (!move) continue;
            executeBotMove(g, p, move);
            played = true;
            break;
        }
        if (!played) break;
    }
    return g;
}

test('the same deal played twice is the same game, log rows included', () => {
    const seed = dealSeed();
    const hex = [...seed].map((b) => b.toString(16).padStart(2, '0')).join('');

    __setEngineClock(() => FIXED_MS);
    try {
        __setDealSeedOverride(seed);
        const a = playOnce('rf', 4, 40);
        __setDealSeedOverride(seed);
        const b = playOnce('rf', 4, 40);
        __setDealSeedOverride(null);

        assert.ok(a.logs.length > 0, `the run wrote no logs, so it proves nothing (deal ${hex})`);

        // The cards first, so a failure here reads as a rules problem rather
        // than a bookkeeping one.
        assert.deepEqual(
            a.players.map((p) => p.hand), b.players.map((p) => p.hand),
            `two plays of deal ${hex} dealt different hands (seed=${rng.seed}, ${rng.env})`,
        );
        assert.deepEqual(a.table_battles, b.table_battles, `table differs (deal ${hex})`);
        assert.equal(a.deck_length, b.deck_length, `deck differs (deal ${hex})`);

        // Then the rows. This is the half that used to differ on every run:
        // each log carried a fresh UUID and a fresh wall-clock stamp.
        assert.equal(a.logs.length, b.logs.length, `log counts differ (deal ${hex})`);
        assert.deepEqual(
            a.logs, b.logs,
            `the log ROWS differ between two plays of deal ${hex} (seed=${rng.seed}, ${rng.env}). `
            + 'If the diff is only `id` or `created_at`, something went back to drawing them: '
            + 'see derivedUuid in sdk/ts/wire/detid.ts and __setEngineClock in sdk/ts/wasm/engine.ts.',
        );

        // Whole-object equality, so a field added later is covered without
        // anyone remembering to come back here.
        assert.deepEqual(a, b, `the two games differ somewhere outside the fields named above (deal ${hex})`);
    } finally {
        __setEngineClock(null);
        __setDealSeedOverride(null);
    }
});

test('log ids are unique within a game and stable across a rerun', () => {
    const seed = dealSeed();
    __setEngineClock(() => FIXED_MS);
    try {
        __setDealSeedOverride(seed);
        const g = playOnce('uniq', 3, 40);
        const ids = g.logs.map((l) => l.id);
        assert.equal(new Set(ids).size, ids.length, `a game reused a log id (seed=${rng.seed})`);
        // A UUID shape, because these go into Postgres `uuid` columns.
        for (const id of ids) {
            assert.match(id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/, `not a v4-shaped id: ${id}`);
        }
        // A DIFFERENT game id gives different ids, so two games' rows never
        // collide even though neither drew anything.
        __setDealSeedOverride(seed);
        const other = playOnce('uniq2', 3, 40);
        assert.equal(
            new Set([...ids, ...other.logs.map((l) => l.id)]).size, ids.length + other.logs.length,
            'two games with the same deal produced colliding log ids',
        );
    } finally {
        __setEngineClock(null);
        __setDealSeedOverride(null);
    }
});
