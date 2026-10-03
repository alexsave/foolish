// live_page's virtual clock cancels the real timers that were armed before it.
//
// The page and the server run on one virtual clock from mount() on, but some
// timers are armed on the real one before it: the pg pool gives every client
// it releases a 10 s idle timer, so the clients the test's setup released
// (applySchema, seedLobby, runMeta) carry real ones into the page's run. When
// the server checks such a client out again, pg-pool cancels its idle timer
// with the global clearTimeout - the virtual clock's by then. That clear used to
// drop a real handle on the floor, the real timer stayed armed, and ten real
// seconds after the release pg-pool's idle callback ended the client under
// whatever query it was running. In the -O0 coverage lane a run passes ten real
// seconds, and web_bot_first_move_pace lost its first bot commit's connection
// after the row was written: the bot loop threw before its broadcast, so the
// page never flew the attack (main run 37137695870).
//
// The timers here are armed with node:timers, the real clock the process
// started with, exactly the handle pg-pool holds for a client released before
// mount, and cleared with the globals the page clock installs.

import { LivePage } from './helpers/live_page.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as realSetTimeout, setInterval as realSetInterval, clearInterval as realClearInterval } from 'node:timers';
import { applySchema, resetDb, uuid } from './harness.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';

before(async () => { await applySchema(); });

const realSleep = (ms: number) => new Promise<void>((r) => { realSetTimeout(r, ms); });

test('the page clock\'s clearTimeout and clearInterval cancel a timer armed on the real clock, as a pool client released before mount holds', async () => {
    await resetDb();
    const gameId = `c${uuid().slice(0, 7)}`;
    const me = uuid();
    await seedLobby(gameId, [{ id: me, name: 'Me' }, { id: uuid(), name: 'Bot1', brain: 'robusta' }]);
    await runMeta(gameId, me, { type: 'update-name', new_name: 'Clock' });

    const page = new LivePage();
    await page.mount(gameId, me);
    try {
        const fired: string[] = [];
        const timeout = realSetTimeout(() => { fired.push('timeout'); }, 20);
        const interval = realSetInterval(() => { fired.push('interval'); }, 20);
        // The page clock's own globals, as pg-pool calls them on a checkout.
        clearTimeout(timeout);
        clearInterval(interval);
        await realSleep(120);
        realClearInterval(interval);   // whatever the outcome, never leave it running past the test
        assert.deepEqual(fired, [], 'a real timer the page clock was asked to cancel still fired');
    } finally {
        await page.unmount();
    }
});
