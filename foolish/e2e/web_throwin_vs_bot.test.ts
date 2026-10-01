// A throw-in I make on the website, against a bot defender that answers the bout.
//
// Owner report: "In the website specifically, I sometimes notice that bots
// actions like 'supercede' human actions. I've literally thrown a card in, then
// had it reverted like a while later by a bot move."
//
// Played end to end (e2e/helpers/live_page.ts): the deployed page, signed in as
// me, attacks and then throws in a second card of the same rank; the real action
// edge function commits each move and wakes the real bot loop, and the loop's
// broadcasts reach the page's own channel. Page and server share one virtual
// clock, with a small fixed network latency each way.
//
// The throw-in is made at the first frame on which my attack has LANDED on my
// screen, before the page has shown any bot move at all: on the board I can see,
// the bout is open, I am its attacker and the rank matches. A bot defender whose
// answer I have not been shown yet must not be able to take that bout away from
// under a move I made on it.

import { LivePage, delivered, flightTimeline, now, probe, sent, type PageFrame } from './helpers/live_page.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid } from './harness.ts';
import { fixture, PLAYING } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';

before(async () => { await applySchema(); });

const card = (t: string) => ({ suit: 'shcd'.indexOf(t[1]), value: '23456789TJQKA'.indexOf(t[0]) + 1 });
// A network a little quicker than production's (an edge round trip there is
// 150-400ms): a slower one only widens the window this case is about.
const LATENCY = { invokeMs: 80, realtimeMs: 40 };

// NOT YET: the fix needs the board's clock persisted in the state blob (v3) and
// the bot loop waiting on it (table_bot_wait_ms). The expand step still writes v2
// (c/src/view.h STATE_BLOB_FORMAT) and the loop still sleeps its old fixed pace;
// the switch step, PR #246, does both and removes this skip.
const NOT_YET = 'needs the v3 state blob and the bot wait that PR #246 (the switch step) turns on';
test('a throw-in I make while my screen shows the bout open is not refused because of a bot move I had not been shown', { skip: NOT_YET }, async () => {
    await resetDb();
    __clearGameCache();
    const gameId = `t${uuid().slice(0, 7)}`;
    const me = uuid();
    // Three seats: I attack, a bot defends holding nothing that beats a nine
    // (hearts are trump and it holds none), and a second bot sits after it.
    await seedTable(gameId, fixture()
        .seats([{ id: me, name: 'Me' }, { id: uuid(), name: 'Def', brain: 'robusta' }, { id: uuid(), name: 'Co', brain: 'robusta' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '9c 9d Jd Qs Kh 7s').hand(1, '6s 7c 6d 7d 8d 6c').hand(2, '9s 8c Tc Jc Qc Kc')
        .deck('8h 9h Th 8s Ts').trump('Ah').discard(12)
        .build(), { views: true, version: 1 });

    const page = new LivePage();
    await page.mount(gameId, me, LATENCY);
    try {
        await page.advance(200);
        const t0 = now();
        await page.step(() => { probe.anim.attack([card('9c')]).catch(() => {}); });

        // Wait for 9c to land on my table.
        const landed = (f: PageFrame) => !f.flying && f.table.some((p) => p.split('/')[0] === '9c');
        assert.ok(await page.advanceUntil(landed, 3000), 'my attack landed on my table');
        const tapAt = now();
        const shownBefore = page.frames.filter((f) => f.t <= tapAt && f.flying && f.flying.seat !== 0);
        assert.deepEqual(shownBefore.map((f) => f.flying), [], 'sanity: no bot move was on my screen before the throw-in');

        let refusal: string | null = null;
        await page.step(() => {
            probe.anim.attack([card('9d')]).then(() => {}, (e: Error) => { refusal = e.message; });
        });
        await page.advance(6000);

        process.stderr.write(`[throw-in] tapped 9c at +0, 9d at +${tapAt - t0}ms\n`
            + `[throw-in] requests: ${sent.filter((s) => s.kind === 'packed').map((s) => `+${s.at - t0}ms -> answered +${s.answeredAt - t0}ms`).join(', ')}\n`
            + `[throw-in] pushes the server sent me: ${delivered.map((d) => `v${d.version} sent +${d.sentAt - t0} received +${d.deliveredAt - t0}`).join(', ')}\n`
            + `[throw-in] what my page flew:\n${flightTimeline(page.frames.filter((f) => f.t >= t0)).join('\n')}\n`);

        const homeFlight = page.frames.find((f) => f.flying?.type === 'revert' && f.flying.cards.includes('9d'));
        assert.equal(refusal, null,
            `the server refused the throw-in I made on the open bout my screen showed (${refusal})`
            + (homeFlight ? `, and my 9d flew back to my hand at +${homeFlight.t - t0}ms, ${homeFlight.t - tapAt}ms after I threw it` : ''));
        assert.equal(homeFlight, undefined, 'my accepted throw-in never flies back to my hand');
    } finally {
        await page.unmount();
    }
});
