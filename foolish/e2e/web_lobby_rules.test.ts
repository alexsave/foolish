// The lobby's rules box on the website, two seats and a spectator, end to end
// (docs/PODKIDNOY.md "The web lobby").
//
// Played through e2e/helpers/live_page.ts: the deployed page (ServerProvider,
// RealtimeAnimationFeed, GameView and so the real Lobby.tsx) signed in as each
// person in turn, against the real meta edge function and the real Postgres, and
// the server's realtime pushes delivered to the page's own channel. One page is
// mounted at a time (the helper owns one virtual clock); the other person's
// moves go through the same real handler (runMeta), which is what their page
// would have sent.
//
// What is pinned: the box sends `set-rules` with `passing: false` when Ana
// unticks it; Bo, the other seat, reads the box unticked from his stored view
// and follows a later change over the push, all BEFORE he readies; a spectator
// sees the box and cannot move it; and when both have readied the dealt game is
// podkidnoy. Who may move the box is the kernel's rule (ViewRules.canSetRules),
// read by the page and never restated - the server-side half is
// e2e/meta_set_rules.test.ts, and the board's missing Pass pill is
// e2e/ui_dom_snapshots.test.ts.

import { LivePage, probe, sent } from './helpers/live_page.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool, resetDb, uuid } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { MAX_PLAYERS } from '../server/api/core/constants.ts';
import { fixture, IDLE } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { runMeta } from './helpers/table_server.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';

before(async () => { await applySchema(); });

/** The rules box as the page drew it: its native control, or a failure naming what the lobby rendered. */
function box(page: LivePage): HTMLInputElement {
    const input = page.host.querySelector<HTMLInputElement>('.lobby__rules input[type="checkbox"]');
    assert.ok(input, `the lobby draws the rules box (${page.host.textContent?.slice(0, 160)})`);
    return input;
}

/** Move the page's clock on until `pred` holds, or `maxMs` of it has passed. */
async function until(page: LivePage, pred: () => boolean, maxMs: number): Promise<void> {
    for (let t = 0; t < maxMs && !pred(); t += 50) await page.advance(50);
}

const passingFlag = async (gameId: string): Promise<boolean> =>
    ((await mustReadTable(gameId)).state[1] & L.TABLE_STATE_FLAG_PASSING) !== 0;

test('web lobby: a seat unticks the rules box, the other seat and a spectator read it before anyone readies, and the deal is podkidnoy', async () => {
    await resetDb();
    __clearGameCache();
    const gameId = `wr${uuid().slice(0, 6)}`;
    const ana = uuid(), bo = uuid(), sam = uuid();
    // A FULL table - Ana, Bo and six bots - so the spectator stays one: a page that
    // opens a lobby with room in it joins it (ServerContext joinOrSubscribe).
    const bots = Array.from({ length: MAX_PLAYERS - 2 }, (_, i) => ({ id: uuid(), name: `%Bot ${i + 1}`, brain: 'random' }));
    await seedTable(gameId, fixture().title('Rules').seats([{ id: ana, name: 'Ana' }, { id: bo, name: 'Bo' }, ...bots])
        .seatStatus(0, IDLE).seatStatus(1, IDLE).build(), { views: true, version: 1 });
    // The spectator is a signed-in somebody with no seat.
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [sam]);

    // ---- Ana, seated: the box is ticked (a new table is the passing game) and hers to move.
    const page = new LivePage();
    await page.mount(gameId, ana);
    try {
        assert.equal(box(page).checked, true, 'a new table: the box is ticked, the passing game');
        assert.equal(box(page).disabled, false, 'a seated player in the lobby may move it');
        await page.step(() => { box(page).click(); });
        const setRules = sent.filter((s) => s.name === 'meta' && s.kind === 'set-rules');
        assert.equal(setRules.length, 1, `unticking sends one set-rules (${sent.map((s) => s.kind).join(',')})`);
        assert.equal((setRules[0].body as { passing?: unknown }).passing, false, 'and it asks for podkidnoy: passing false');
        assert.equal(setRules[0].status, 200, 'the server takes it');
        assert.equal(box(page).checked, false, "Ana's box now reads unticked");
        await page.advance(1500);
        assert.equal(box(page).checked, false, "and stays unticked once the server's push of it has landed");
        assert.equal(await passingFlag(gameId), false, 'the stored table is podkidnoy');
    } finally {
        await page.unmount();
    }

    // ---- Sam, a spectator: he sees which game he would be joining, and cannot change it.
    const spectator = new LivePage();
    await spectator.mount(gameId, sam);
    try {
        assert.equal(probe.view?.mySeat, -1, 'Sam holds no seat: the table is full');
        assert.equal(box(spectator).checked, false, 'the spectator reads podkidnoy');
        assert.equal(box(spectator).disabled, true, 'and the box is not his to move');
        const before = sent.length;
        await spectator.step(() => { box(spectator).click(); });
        assert.equal(sent.slice(before).filter((s) => s.kind === 'set-rules').length, 0, "a click on a spectator's box sends nothing");
        assert.equal(box(spectator).checked, false, 'and it still reads podkidnoy');
    } finally {
        await spectator.unmount();
    }

    // ---- Bo, the other seat: the stored view says podkidnoy before he readies, and the
    // push carries the next change, both ways.
    const other = new LivePage();
    await other.mount(gameId, bo);
    try {
        const me = probe.view.seats[probe.view.mySeat];
        assert.equal(me.status, L.PLAYER_STATUS_IDLE, 'Bo has not readied');
        assert.equal(box(other).checked, false, "Bo's lobby opens on Ana's choice: podkidnoy");
        assert.equal(box(other).disabled, false, 'and Bo, seated, may change it too');

        await other.step(async () => { await runMeta(gameId, ana, { type: 'set-rules', passing: true }); });
        // A lobby push is one step (the generic transition every lobby change
        // plays), and the board it carries is applied when that step settles.
        await until(other, () => box(other).checked, 3000);
        assert.equal(box(other).checked, true, "Ana ticks it again and Bo's box follows over the push");
        await other.step(async () => { await runMeta(gameId, ana, { type: 'set-rules', passing: false }); });
        await until(other, () => !box(other).checked, 3000);
        assert.equal(box(other).checked, false, 'and back to podkidnoy, still before Bo readies');

        // Both ready - Bo from his page, Ana through the same handler - and the last Ready deals.
        const ready = other.host.querySelector('.btn-ready');
        assert.ok(ready, 'Bo has a Ready button');
        await other.step(() => { ready.dispatchEvent(new (globalThis as any).window.MouseEvent('click', { bubbles: true })); });
        assert.ok(sent.some((s) => s.kind === 'start' && s.status === 200), "Bo's Ready reaches the server");
        await other.step(async () => { await runMeta(gameId, ana, { type: 'start' }); });
        await other.advance(2000);

        const dealt = await mustReadTable(gameId);
        assert.equal(dealt.status, L.GAME_STATUS_PLAYING, 'the last Ready deals');
        assert.equal(dealt.state[1] & L.TABLE_STATE_FLAG_PASSING, 0, 'and the dealt game is podkidnoy');
        assert.equal(probe.view.status, L.GAME_STATUS_PLAYING, "Bo's page is on the dealt board");
        assert.equal(probe.view.passing, false, "and the board Bo plays on reads podkidnoy");
        assert.equal(other.host.querySelector('.lobby__rules'), null, 'the lobby, and its box, are gone');
    } finally {
        await other.unmount();
    }
});
