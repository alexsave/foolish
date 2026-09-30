// The website's FIRST bot move of a game, as a human watching it sees it.
//
// Owner report: "the FIRST move in the game, if it is by a bot, it just goes
// super quick and seems to not at all wait for any other players. Its like a bot
// attacks, then the defender bot INSTANTLY picks up with zero bot delay."
//
// Played end to end: the deployed page (e2e/helpers/live_page.ts) presses Start
// on a lobby of me and two bots; the real meta edge function deals and wakes the
// real bot loop (scheduleBotLoop -> lockedBotLoop), whose broadcasts reach the
// page's own channel; the page and the server share one virtual clock, so the
// loop's pacing sleep and the page's animation plan are measured on the same
// milliseconds. The number the gap is held to is the kernel's own pace for the
// attack's cycle (bot_cycle_delay_ms, read off the loop's table as it runs), not
// a constant restated here.

import { LivePage, delivered, flightTimeline, now, probe } from './helpers/live_page.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid } from './harness.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { serverTable } from '../sdk/ts/table/server_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

before(async () => { await applySchema(); });

// The pace the loop's table reports for each cycle it drives, in order: the
// kernel's answer (bot_cycle_delay_ms), recorded where lockedBotLoop reads it.
const paces: { at: number; ms: number }[] = [];
before(async () => {
    const table = await serverTable();
    const cycleDelayMs = table.cycleDelayMs.bind(table);
    table.cycleDelayMs = () => { const ms = cycleDelayMs(); paces.push({ at: now(), ms }); return ms; };
});

// A deal whose first attacker AND defender are both bots, with me watching from
// the third seat. The deal is the kernel's from a fixed seed, so the case is the
// same every run.
const DEAL_SEED = Uint8Array.from({ length: 32 }, (_, i) => (i * 7 + 13) & 255);

test('the first bot move of a game: the defender bot answers a full pace after the attack appears on my screen', async () => {
    await resetDb();
    __clearGameCache();
    __setTableDealSeedOverride(DEAL_SEED);
    paces.length = 0;
    const gameId = `f${uuid().slice(0, 7)}`;
    const me = uuid();
    await seedLobby(gameId, [
        { id: me, name: 'Me' },
        { id: uuid(), name: 'Bot1', brain: 'robusta' },
        { id: uuid(), name: 'Bot2', brain: 'robusta' },
    ]);
    // A seeded row has no per-viewer views yet; the host naming the table is a
    // real commit, and commit_table writes the view the page loads.
    await runMeta(gameId, me, { type: 'update-name', new_name: 'First move' });

    const page = new LivePage();
    await page.mount(gameId, me);
    try {
        const startedAt = now();
        // The lobby's Start button calls exactly this (ServerContext startGame).
        await page.step(() => { probe.actions.startGame(gameId).catch(() => {}); });
        const dealt = await mustReadTable(gameId);
        assert.equal(dealt.status, L.GAME_STATUS_PLAYING, 'the Start press dealt the game');
        const attacker = dealt.firstAttacker, defender = dealt.defender;
        assert.ok(attacker !== 0 && defender !== 0, `fixture: both roles are bots (attacker ${attacker}, defender ${defender})`);

        // Watch the page until the defender's answer has flown.
        const isAnswer = (f: { flying: { type: string; seat: number | undefined } | null }) =>
            !!f.flying && f.flying.seat === defender && (f.flying.type === 'cover' || f.flying.type === 'pickup');
        const answered = await page.advanceUntil(isAnswer, 20_000);
        await page.advance(1000);
        process.stderr.write(`[first-move] seats: me 0, attacker ${attacker}, defender ${defender}\n`
            + `[first-move] pushes the server sent me (ms after Start): ${delivered.map((d) => `v${d.version}@+${d.sentAt - startedAt}`).join(', ')}\n`
            + `[first-move] kernel pace per bot cycle: ${paces.map((p) => `${p.ms}ms (cycle at +${p.at - startedAt})`).join(', ')}\n`
            + `[first-move] what my page flew:\n${flightTimeline(page.frames).join('\n')}\n`);
        assert.ok(answered, 'the defender bot answered the first attack');

        // When the attack became visible on my page (its flight opened) and when
        // the defender's answer did. The kernel paces the loop cycle to cycle, so
        // two bot moves in a row are one pace apart on the server's clock; this
        // asks the same of the clock my screen runs on.
        const frames = page.frames;
        const isAttack = (f: (typeof frames)[number]) => f.flying?.type === 'attack_pass' && f.flying.seat === attacker;
        const attackOpen = frames.find(isAttack);
        assert.ok(attackOpen, 'the page flew the first attack');
        const answerOpen = frames.find(isAnswer)!;
        const gap = answerOpen.t - attackOpen.t;
        const attackPace = paces[0]?.ms ?? 0;
        assert.ok(attackPace > 0, `the kernel paced the attack's cycle (${attackPace}ms)`);
        assert.ok(gap >= attackPace,
            `the defender bot's ${answerOpen.flying!.type} started moving ${gap}ms after the first attack did on my screen `
            + `(attack at ${attackOpen.t}ms, answer at ${answerOpen.t}ms after Start); the kernel's pace between those two cycles is ${attackPace}ms`);
    } finally {
        await page.unmount();
        __setTableDealSeedOverride(null);
    }
});
