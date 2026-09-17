// A bot seat plays the brain you ASKED for, or is refused.
//
// This file used to hold the move-for-move parity of the kernel's random,
// handwritten and simple_heuristic brains against their frozen TypeScript
// originals (offlinefun/localtest/frozen) on a TypeScript Game. That oracle is
// retired with the TS game shape (docs/C_GAME_SHAPE_MIGRATION.md Phase 8, Q11):
// the kernel's brains are held by the C difftests (make -C c difftests) and the
// native suite, and the server plays them through the C Table only.
//
// What stays is the identity property the registry exists for. A key that
// resolved to `random` silently is how a CI gate measured `random` under two
// culled bots' names for months (issue #111), and how a typo'd key would become
// a random bot on the Elo ladder. On the C Table there is no fallback: a brain
// this build does not link is TABLE_E_UNKNOWN_BRAIN, both where a lobby adds a
// bot (table_add_bot) and where a stored row is loaded (table_load, here through
// the fixture's seal).
//
// Pure kernel test - needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { kernelBotRoster, kernelBotStrat } from '../sdk/ts/wasm/bots.ts';
import { fixture, fixtureTable, FixtureRefused } from './helpers/table_fixture.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const SEED = new Uint8Array(32);
const lobby = fixture().seats([{ id: 'host', name: 'Host' }]).build();

/** What table_add_bot says of `brain`, and the brain the seat it added holds. */
function addBot(brain: string): { rc: number; seated: string | null } {
    const table = fixtureTable();
    assert.equal(table.load(lobby.state, lobby.roster), L.TABLE_OK, 'the lobby loads');
    const rc = table.addBot('host', 'bot-1', 'Bot', brain, SEED);
    const seats = table.seats();
    return { rc, seated: seats.length > 1 ? seats[1].brain : null };
}

/** What table_load (the fixture's seal) says of a stored row seating `brain`. */
function loadRow(brain: string): number {
    try {
        fixture().seats([{ id: 'host', name: 'Host' }, { id: 'bot-1', name: 'Bot', brain }]).build();
        return L.TABLE_OK;
    } catch (e) {
        if (e instanceof FixtureRefused) return e.code;
        throw e;
    }
}

test('every brain this build links seats as itself', () => {
    const linked = kernelBotRoster().filter((e) => e.linked);
    assert.ok(linked.length >= 4, `the roster links the shipped bots (${linked.map((e) => e.key).join(', ')})`);
    for (const { key, strat } of linked) {
        assert.equal(kernelBotStrat(key), strat, `${key} resolves to its own brain`);
        const r = addBot(key);
        assert.equal(r.rc, L.TABLE_OK, `${key}: added to a lobby`);
        assert.equal(r.seated, key, `${key}: the seat holds the brain asked for, not a neighbour or random`);
        assert.equal(loadRow(key), L.TABLE_OK, `${key}: a stored row seating it loads`);
    }
});

test('a culled, unlinked or never-existing key is refused, never seated as random', () => {
    // A culled bot (semtex/fulminate were real once), a roster row this build does
    // not link, and names that never existed are the same thing to the table.
    const unlinked = kernelBotRoster().filter((e) => !e.linked).map((e) => e.key);
    const linked = kernelBotRoster().filter((e) => e.linked).map((e) => e.key);
    for (const dead of ['semtex', 'fulminate', 'totallyfakebot', 'RANDOM', ...unlinked]) {
        assert.ok(!linked.includes(dead), `'${dead}' is not a linked brain`);
        const r = addBot(dead);
        assert.equal(r.rc, L.TABLE_E_UNKNOWN_BRAIN, `table_add_bot('${dead}') must refuse, not seat a stand-in`);
        assert.equal(r.seated, null, `'${dead}': nobody was seated`);
        assert.equal(loadRow(dead), L.TABLE_E_UNKNOWN_BRAIN, `a stored row seating '${dead}' must not load`);
    }
    assert.equal(addBot('').rc, L.TABLE_E_UNKNOWN_BRAIN, 'a bot with no brain is refused');
});
