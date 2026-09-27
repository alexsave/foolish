// A game's mid-game draws are seeded from ITS deal seed, whatever game the warm
// isolate served before.
//
// The kernel seeds every mid-game draw from the board and the table's rng base
// (table.c table_act: game_state_seed(g, rng_base)), and the rng base is the deal
// seed the host hands table_set_deal_seed. bots.wasm keeps one base per
// instance: table_load starts a table from whatever base the instance last set,
// which on a warm edge isolate is another game's. The bot loop sets the base
// after every load; a human move and a lobby edit (table_io.ts runTableOp) must
// too, or a draw's outcome depends on which game the isolate served last, and
// the game no longer replays from its own seed.
//
// The draw that reads the base is a legacy deck's (a game dealt before decks
// were pre-shuffled from the deal seed: game.c draw_index). Two identical such
// games, the same moves, each served right after a bot cycle of a different
// game, must draw the same cards.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { legalMoves, mustReadTable, type TableState } from './helpers/table_play.ts';
import { runAction } from './helpers/table_server.ts';
import { ACTION_STATUS } from '../sdk/ts/wire/awire.ts';
import { __botCycle } from '../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { await pgPool.end(); });

const seedHex = (byte: number) => byte.toString(16).padStart(2, '0').repeat(32);

/** A game whose bot (seat 1) attacks next: a bot cycle on it sets the instance's base to `seed`. */
async function botGame(seed: string): Promise<string> {
    const id = `ra${uuid().slice(0, 6)}`;
    await seedTable(id, fixture()
        .seats([{ id: uuid(), name: 'H' }, { id: uuid(), name: 'B', brain: 'random' }])
        .status(PLAYING).attacker(1).defender(0).deterministic()
        .hand(0, '6s 7s 8s 9s Ts Js').hand(1, '6c 7c 8c 9c Tc Jc').trump('9d').deck('Ad Kd Qd').build());
    await pgPool.query('UPDATE games SET game_seed = $2 WHERE id = $1', [id, seed]);
    return id;
}

/** Two humans on a legacy deck: seat 0 has attacked with 6h and holds two cards, seat 1 defends. */
const HUMANS = [uuid(), uuid()];
async function legacyDeckGame(): Promise<string> {
    const id = `rb${uuid().slice(0, 6)}`;
    await seedTable(id, fixture()
        .seats([{ id: HUMANS[0], name: 'H0' }, { id: HUMANS[1], name: 'H1' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '7c 8c').hand(1, '9s Ts Js Qs Ks As').table('6h').trump('9d')
        .deck('6d 7d 8d Td Jd Qd Kd Ad 6s 7s 8s 6c 9c Tc Jc Qc Kc Ac 7h 8h').build());
    await pgPool.query('UPDATE games SET game_seed = $2 WHERE id = $1', [id, seedHex(0x5b)]);
    return id;
}

/** The defender picks up and the round is closed, move by move, until the attacker has drawn. */
async function pickupRound(id: string): Promise<TableState> {
    const start = await mustReadTable(id);
    for (let step = 0; step < 8; step++) {
        const t = await mustReadTable(id);
        if (t.deckCount !== start.deckCount) return t;
        const moves = legalMoves(t);
        const mv = moves.find((m) => m.kind === 'pickup') ?? moves.find((m) => m.kind === 'good');
        assert.ok(mv, `fixture: a pickup or a good is legal (step ${step}: ${moves.map((m) => m.kind).join(', ')})`);
        assert.equal((await runAction(id, mv.playerId, mv)).status, ACTION_STATUS.APPLIED);
    }
    assert.fail('fixture: the round never drew');
}

test('a human move\'s draws come from its own game\'s seed, not from the game the instance served before', async () => {
    const outcomes: TableState[] = [];
    for (const other of [0x11, 0xee]) {
        // The warm isolate runs another game's bot cycle (its seed becomes the instance's base) ...
        const a = await botGame(seedHex(other));
        const aBefore = (await mustReadTable(a)).version;
        await __botCycle(a);
        assert.equal((await mustReadTable(a)).version, aBefore + 1, 'fixture: the other game\'s bot moved');
        // ... and then serves this game's human moves.
        const b = await legacyDeckGame();
        assert.equal((await mustReadTable(b)).deterministic, false, 'fixture: a legacy deck, whose draws read the seed');
        outcomes.push(await pickupRound(b));
    }
    const hands = outcomes.map((t) => t.seats.map((s) => s.hand.map((c) => `${c.suit}:${c.value}`)));
    assert.equal(outcomes[0].deckCount, outcomes[1].deckCount, 'the same number of cards drawn');
    assert.ok(outcomes[0].deckCount < 20, 'fixture: the round drew');
    assert.deepEqual(hands[1], hands[0], 'the same game and the same moves draw the same cards, whichever game the instance served before');
    assert.equal(Buffer.from(outcomes[1].state).toString('hex'), Buffer.from(outcomes[0].state).toString('hex'));
    assert.equal(outcomes[0].status, L.GAME_STATUS_PLAYING);
});
