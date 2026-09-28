// SECURITY S1 at the 2-seat table: two humans, no bots. A whole game is played
// through the real edge handlers, and after every step every payload each viewer
// (both seats, a signed-in spectator, anon) can receive is proved to hide what
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
//
// The two realtime channel tests are here rather than in a file of their own:
// their fixture is a lobby of two seated humans and a spectator, which is this
// file's cast, and they cost nothing (14ms and 6ms together) so they add nothing
// measurable to the file that is already the longest of the three.

import './harness.ts';
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { settle } from './helpers/edge.ts';
import { installHiddenInfoSchema, playTable, assertExercised, channelFixture } from './helpers/hidden_info.ts';

before(async () => { await installHiddenInfoSchema(); });
after(async () => { await settle(); });

test('2 seats (two humans): every payload each viewer receives hides what it must', async (tc) => {
    const t = await playTable(2, 0, 2);
    tc.diagnostic(`${t.gameId} ${JSON.stringify(t.counts)}`);
    await assertExercised(t, '2p');
});

test('realtime: nobody but its owner can join a gu- topic; anon cannot join game-; a signed-in spectator can', async () => {
    const { a, b, spectator, gameId, canJoin } = await channelFixture();
    const guA = `gu-${gameId}-${a.id}`;
    assert.equal(await canJoin('authenticated', b.id, guA), false, 'another seated player cannot join A\'s gu- topic');
    assert.equal(await canJoin('authenticated', spectator.id, guA), false, 'a spectator cannot join A\'s gu- topic');
    assert.equal(await canJoin('anon', null, guA), false, 'anon cannot join A\'s gu- topic');
    assert.equal(await canJoin('anon', null, `game-${gameId}`), false, 'anon cannot join the game- topic');
    assert.equal(await canJoin('authenticated', spectator.id, `game-${gameId}`), true, 'a signed-in spectator can join the game- topic');
});

// The owner half of the matrix above. seed.sql's gu- policy used to compare
// split_part(topic, '-', 3) with auth.uid()::text; a user id is a hyphenated
// UUID, so that field is only its first 8 hex digits and every gu- join was
// refused, the owner's included (fail-closed, not a leak). The policy now
// rebuilds the exact topic from the player_hands row
// (seed.sql's realtime policies; migration 20260917120000 carried the same
// change to hosted).
test('realtime: a seated player can join their OWN gu- topic', async () => {
    const { a, b, gameId, canJoin } = await channelFixture();
    assert.equal(await canJoin('authenticated', a.id, `gu-${gameId}-${a.id}`), true, 'A can join A\'s own gu- topic');
    assert.equal(await canJoin('authenticated', b.id, `gu-${gameId}-${b.id}`), true, 'B, who joined the lobby, can join B\'s own gu- topic');
});
