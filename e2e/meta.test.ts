// E2E for the consolidated `meta` endpoint: the REAL handler (meta_actions.ts,
// the code meta/index.ts dispatches: one C Table operation per body.type)
// through the REAL CAS commit (commit_table) and the pg adapter, on kernel-owned
// rows. Results are read back through the kernel (helpers/table_play.ts).
//
// Owns the meta validation scenarios; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerMetaValidation` and
// provides the shared DB before/after.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { GAME_STATUS, PLAYER_STATUS } from '../server/api/core/types.ts';
import { resetToLobby } from '../src/state/clientReconcile.ts';
import { gameToView } from './helpers/view_game.ts';
import { fixture, fixtureTable, GAME_OVER, IN, OUT } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { checkCardConservation, mustReadTable, readTable, residentBoard } from './helpers/table_play.ts';
import { playToEnd, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

// Only one test here draws: the rematch scenario plays a dealt game out with
// random legal moves. Seeded, so the game it plays is the same game every run.
const rng = suiteRng('meta');

const human = (id: string, name: string, ready = true) => ({ id, name, ready });

// ---- handpicked validation: a representative deal + a reject -----------------
export function registerMetaValidation(): void {
    test('meta:start - when all players are ready the game deals and conserves cards', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid();
        await seedLobby(gameId, [human(h1, 'H1', false), human(h2, 'H2')]);
        await runMeta(gameId, h1, { type: 'start' });
        const t = await mustReadTable(gameId);
        assert.equal(t.status, L.GAME_STATUS_PLAYING, 'game started');
        assert.equal(t.statusColumn, 'playing', 'and the column says so');
        assert.ok((await checkCardConservation(gameId)).ok, 'cards conserved on deal');
    });

    test('meta: unknown type is rejected', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);
        await assert.rejects(runMeta(gameId, h1, { type: 'nonsense' }), /unknown meta action/i);
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    test('meta:add-bot - adds a bot and (all ready) starts the game', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [uuid(), 'Botty', 'random']);

        const res = await runMeta(gameId, h1, { type: 'add-bot' });
        const t = await mustReadTable(gameId);
        assert.equal(t.seats.length, 2, 'bot added');
        assert.equal(t.seats.filter(s => s.brain).length, 1, 'one bot');
        assert.equal(t.status, L.GAME_STATUS_PLAYING, 'all ready -> started');
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '1', 'the bot is a member');
        assert.equal(res.runBots === gameId, t.needsBotsColumn, 'the bots are woken exactly when the kernel says they have work');
    });

    test('meta:add-bot - a specific bot_id adds exactly that bot', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), b1 = uuid(), b2 = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3),($4,$5,$6)',
            [b1, 'Botty1', 'random', b2, 'Botty2', 'random']);

        await runMeta(gameId, h1, { type: 'add-bot', bot_id: b2 });
        const bots = (await mustReadTable(gameId)).seats.filter(s => s.brain);
        assert.equal(bots.length, 1, 'one bot added');
        assert.equal(bots[0].id, b2, 'the requested bot (b2), not a random one');
        assert.equal(bots[0].name, 'Botty2', 'seated under its nickname');
    });

    test('meta:add-bot - an unavailable bot_id is rejected', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [uuid(), 'Botty', 'random']);
        await assert.rejects(runMeta(gameId, h1, { type: 'add-bot', bot_id: uuid() }), /not available/i);
    });

    test('meta:exit - removing a bot drops it; removing the last player deletes the game', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), bot = uuid();
        await seedLobby(gameId, [human(h1, 'H1', false), { id: bot, name: 'Botty', brain: 'random' }]);

        // The bot's membership row goes with the commit that removes it:
        // commit_table prunes bot_hands to the roster in the same transaction.
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '1', 'bot member before removal');
        await runMeta(gameId, h1, { type: 'exit', bot_id: bot });
        assert.equal((await mustReadTable(gameId)).seats.length, 1, 'bot removed');
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '0', 'bot membership pruned by commit_table');

        // The last exit must RESOLVE, not just happen to delete the row.
        await runMeta(gameId, h1, { type: 'exit' });
        assert.equal(await readTable(gameId), null, 'empty game deleted');
    });

    // The client applies "proceed to lobby" OPTIMISTICALLY (resetToLobby) before
    // the meta round-trip; the authoritative reset (table_continue, the kernel's
    // game_reset_to_lobby) must agree on the public fields, or the user sees a
    // snap. Both run on the same finished game.
    test('optimistic resetToLobby (client) matches table_continue (server)', () => {
        const players = [
            { player_id: 'h1', name: 'H1', is_ai: false, status: PLAYER_STATUS.OUT, hand_length: 0 },
            { player_id: 'b1', name: 'Botty', is_ai: true, status: PLAYER_STATUS.IN, hand_length: 4 },
        ];
        const clientGame: any = {
            id: 'g1', name: 'G1', status: GAME_STATUS.GAME_OVER,
            discard_pile_length: 7, flipped: { suit: 1, value: 9 },
            power_suit: 1, first_attacker: 1, defender: 0,
            table_battles: [{ attack: { suit: 0, value: 5 }, defense: null }],
            elimination_order: ['h1'], good_timestamp: 123, good_players: ['h1'], version: 41,
            deck_length: 5, players: structuredClone(players),
            self: { player_id: 'h1', name: 'H1', is_ai: false, status: PLAYER_STATUS.OUT, hand: [{ suit: 0, value: 5 }], hand_length: 1, awaiting_attack: true, strategy_key: 'human' },
        };
        // The same finished board, as the kernel holds it.
        const fx = fixture().title('G1')
            .seats([{ id: 'h1', name: 'H1' }, { id: 'b1', name: 'Botty', brain: 'random' }])
            .status(GAME_OVER).seatStatus(0, OUT).seatStatus(1, IN).powerSuit(1).attacker(1).defender(0)
            .hand(1, '6c 7c 8c 9c').eliminated(0).discard(20).good(0).goodTimestamp()
            .build();
        const table = fixtureTable();
        assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK);
        assert.equal(table.continueGame('h1'), L.TABLE_OK, 'the server resets it');
        const server = residentBoard('g1', fx.state, fx.roster);

        // The client holds the finished game as a board (TableView), as the web does.
        const clientView = gameToView(clientGame);
        const client = resetToLobby(clientView);
        assert.equal(client.status, L.GAME_STATUS_WAITING);
        assert.equal(server.status, L.GAME_STATUS_WAITING, 'status matches');
        for (let i = 0; i < client.seats.length; i++) {
            assert.equal(client.seats[i].status, server.seats[i].status, `player ${i} status matches server`);
            assert.equal(client.seats[i].handCount, 0, `player ${i} hand cleared`);
            assert.equal(server.seats[i].hand.length, 0, `seat ${i} hand cleared`);
        }
        assert.deepEqual(client.myHand, [], 'the viewer\'s hand cleared');
        for (const [f, s] of [['discardPileLength', 'discard'], ['powerSuit', 'powerSuit'], ['firstAttacker', 'firstAttacker'], ['defender', 'defender']] as const) {
            assert.equal((client as any)[f], (server as any)[s], `${f} matches server`);
        }
        assert.equal(client.hasFlipped, false);
        assert.equal(server.trump, null);
        assert.deepEqual(client.battles, []);
        assert.deepEqual(server.battles, []);
        assert.deepEqual(client.elimination, []);
        assert.deepEqual(server.eliminated, []);
        assert.equal(client.goodMask, 0, 'client clears the goods');
        assert.equal(server.goodMask, 0, 'the server clears the goods');
        assert.equal(client.hasGoodTimestamp, false, 'client clears the good timestamp');
        assert.equal(client.version, 41, 'version preserved for the reorder gate');
        assert.equal(clientView.status, L.GAME_STATUS_GAME_OVER, 'input not mutated (rollback needs it)');
    });

    test('meta:continue - resets a finished game back to the lobby', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid(), bot = uuid();
        await seedTable(gameId, fixture()
            .seats([{ id: h1, name: 'H1' }, { id: h2, name: 'H2' }, { id: bot, name: 'Botty', brain: 'random' }])
            .status(GAME_OVER).powerSuit(0).eliminated(0, 2).discard(36).build());

        await runMeta(gameId, h1, { type: 'continue' });
        const t = await mustReadTable(gameId);
        assert.equal(t.status, L.GAME_STATUS_WAITING, 'reset to lobby');
        assert.equal(t.statusColumn, 'waiting');
        assert.deepEqual(t.seats.map(s => s.status), [L.PLAYER_STATUS_IDLE, L.PLAYER_STATUS_IDLE, L.PLAYER_STATUS_READY], 'humans idle, bots ready');
    });

    test('meta:join - a new player joins a waiting game and becomes a member', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), joiner = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [joiner]);

        await runMeta(gameId, joiner, { type: 'join' }, 'Joiner');
        const t = await mustReadTable(gameId);
        assert.deepEqual(t.seats.map(s => [s.id, s.name]), [[h1, 'H1'], [joiner, 'Joiner']], 'joiner seated under their name');
        assert.equal((await pgPool.query('SELECT count(*) FROM player_hands WHERE game_id=$1 AND player_id=$2', [gameId, joiner])).rows[0].count, '1', 'joiner membership persisted');

        await assert.rejects(runMeta(gameId, joiner, { type: 'join' }), /already in game/i);
    });

    test('meta:rearrange-players - reorders the lobby seating', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid();
        await seedLobby(gameId, [human(h1, 'H1'), human(h2, 'H2')]);

        await runMeta(gameId, h1, { type: 'rearrange-players', new_order: [h2, h1] });
        assert.deepEqual((await mustReadTable(gameId)).seats.map(s => s.id), [h2, h1], 'order swapped');

        await assert.rejects(runMeta(gameId, h1, { type: 'rearrange-players', new_order: [h1] }), /every seated player exactly once/i);
        await assert.rejects(runMeta(gameId, h1, { type: 'rearrange-players', new_order: [h1, uuid()] }), /every seated player exactly once/i);
    });

    test('meta:update-name - renames the game in the lobby (with validation)', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedLobby(gameId, [human(h1, 'H1')]);

        await runMeta(gameId, h1, { type: 'update-name', new_name: '  Cool Game  ' });
        assert.equal((await mustReadTable(gameId)).title, 'Cool Game', 'name trimmed + saved');

        await assert.rejects(runMeta(gameId, h1, { type: 'update-name', new_name: '   ' }), /1-50 characters/i);
        await assert.rejects(runMeta(gameId, h1, { type: 'update-name', new_name: 'x'.repeat(51) }), /1-50 characters/i);
    });

    test('create_table RPC - creates games + player_hands membership in one call', async () => {
        const gameId = `c${uuid().slice(0, 5)}`;
        const creator = uuid();
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [creator]);
        const table = fixtureTable();
        assert.equal(table.create(creator, 'Creator'), L.TABLE_OK);
        const p = table.commit(gameId, 0, 0);
        assert.ok(typeof p !== 'number');
        const hex = (b: Uint8Array) => `\\x${Buffer.from(b).toString('hex')}`;
        await pgPool.query('SELECT create_table($1,$2,$3,$4)', [gameId, creator, hex(p.state), hex(p.roster)]);

        const t = await mustReadTable(gameId);
        assert.equal(t.statusColumn, 'waiting', 'waiting lobby');
        assert.equal(t.title, "Creator's Game");
        assert.deepEqual(t.seats.map(s => s.id), [creator], 'creator seated');
        assert.equal(t.deckCount, 0, 'no deck');
        assert.equal((await pgPool.query('SELECT count(*) FROM player_hands WHERE game_id=$1 AND player_id=$2', [gameId, creator])).rows[0].count, '1', 'creator membership created');
    });

    // The full rematch cycle on a DEALT game: continue must replace the finished
    // session's blob with the lobby's, the lobby must stay mutable, and a rematch
    // must deal.
    test('meta:continue - full rematch on a dealt game: blob replaced, lobby mutable, restart works', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid(), h3 = uuid();
        await seedLobby(gameId, [human(h1, 'H1', false), human(h2, 'H2')]);
        await runMeta(gameId, h1, { type: 'start' });

        const finished = await playToEnd(gameId, { pick: (m) => rng.pick(m) });
        assert.equal(finished.statusColumn, 'game_over', `game played to completion (seed=${rng.seed})`);

        await runMeta(gameId, h1, { type: 'continue' });
        const reset = await mustReadTable(gameId);
        assert.equal(reset.statusColumn, 'waiting', 'reset to lobby');
        assert.notDeepEqual(reset.state, finished.state, 'the finished blob is replaced');
        assert.ok(reset.seats.every(s => s.hand.length === 0), 'no hands survive into the lobby');
        assert.equal(reset.logsPacked, '', 'the session log is gone with the session');

        // The post-continue lobby must be fully mutable.
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [h3]);
        await runMeta(gameId, h3, { type: 'join' });
        await runMeta(gameId, h2, { type: 'exit' });
        const churned = await mustReadTable(gameId);
        assert.deepEqual(churned.seats.map(s => s.id), [h1, h3], 'join + exit both applied');

        // Everyone readies up: the rematch must actually deal.
        await runMeta(gameId, h1, { type: 'start' });
        await runMeta(gameId, h3, { type: 'start' });
        assert.equal((await mustReadTable(gameId)).statusColumn, 'playing', 'rematch dealt');
        assert.ok((await checkCardConservation(gameId)).ok, 'cards conserved on the rematch deal');
    });

    registerMetaValidation();

    after(async () => { await pgPool.end(); });
}
