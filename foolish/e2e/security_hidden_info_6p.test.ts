// SECURITY S1 at the 6-seat table: three humans and three bots, interleaved
// around the table, the widest shape the scenario plays. Three humans means
// three seats whose hands must be hidden from each other as well as from the
// spectator and anon, and more per-event snapshots per push than the smaller
// tables produce. A whole game is played through the real edge handlers, and
// after every step every payload each viewer can receive is proved to hide what
// it must, three independent ways: the client's decode, the kernel's masked
// re-encode, and a noninterference replay. The scenario, the checkers and the
// ground truth are e2e/helpers/hidden_info.ts; this file only names the table.
//
// One process per seat count, because the three scenarios are independent and
// serialised they set the db lane's floor at 80s. node:test gives a file one
// process and runs its top-level tests one after another, so with the 2-, 4-
// and 6-seat games in one file (29.8s + 27.8s + 21.2s) that file alone was the
// db lane's wall clock in scripts/run_e2e.mjs; as three files the lane runs them
// side by side and the floor drops to the longest. The price is one schema
// build per file, measured at about 0.15s.

import './harness.ts';
import { test, before, after } from 'node:test';
import { settle } from './helpers/edge.ts';
import { installHiddenInfoSchema, playTable, assertExercised } from './helpers/hidden_info.ts';

before(async () => { await installHiddenInfoSchema(); });
after(async () => { await settle(); });

test('6 seats (three humans, three bots): every payload each viewer receives hides what it must', async (tc) => {
    const t = await playTable(3, 3, 6);
    tc.diagnostic(`${t.gameId} ${JSON.stringify(t.counts)}`);
    await assertExercised(t, '6p');
});
