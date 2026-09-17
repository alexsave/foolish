// Realtime private-channel authorization, evaluated the way Supabase Realtime
// evaluates it.
//
// A client joining a channel with `{ config: { private: true } }` is admitted only
// if the RLS SELECT policies on realtime.messages return the probe row Realtime
// writes for that join. Realtime runs that SELECT as the client's role, with the
// client's JWT claims in `request.jwt.claims` and the channel name in the
// `realtime.topic` setting that realtime.topic() reads. This file stands up
// faithful copies of auth.uid() and realtime.topic(), applies seed.sql (and then
// the migration that carries the same policies to the hosted project), and asks
// that question for every topic family the web joins privately:
//
//   gu-{game_id}-{user_id}  per-seat animation stream (RealtimeAnimationFeed)
//   chat:{game_id}          chat inserts (chat_messages_changes trigger)
//   game-{game_id}          spectator animation stream
//
// and that no client can SEND on any of them (only the server broadcasts, with
// the service-role key). The `user-{email local part}` family is gone: nothing
// has joined it since 2025-07, its receive policy let alice@a.com and
// alice@b.com read each other's topic, and any signed-in user could send to any
// user- topic.
//
// The gu- and chat: policies used to cut the topic apart with split_part. A user
// id is a hyphenated UUID, so split_part(topic, '-', 3) is only its first 8 hex
// digits and never equals auth.uid(): every gu- join was refused, the owner's
// included, and the web board received no live animation broadcasts at all.
// The policies now rebuild the exact topic from the membership row instead.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, uuid, pgPool } from './harness.ts';
import { fixture } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';

const MIGRATIONS = join(process.cwd(), 'server', 'impls', 'supabase', 'migrations');
// The migrations that carry this file's policies to hosted, in the order they apply.
const REALTIME_MIGRATIONS = ['_realtime_channel_exact_topics.sql', '_drop_tmp_allow_all_realtime_policy.sql', '_drop_user_realtime_policies.sql']
    .map((suffix) => readdirSync(MIGRATIONS).find((f) => f.endsWith(suffix)));

type Role = 'anon' | 'authenticated';

/** Run `fn` as Realtime runs a channel's authorization: the client's role, its
 *  JWT claims and the channel topic set for the transaction, rolled back after. */
async function asChannel<T>(role: Role, sub: string | null, topic: string, fn: (c: import('pg').PoolClient) => Promise<T>, email?: string): Promise<T> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        // The probe row Realtime inserts for a join, as the owner.
        await c.query(`INSERT INTO realtime.messages(topic, extension) VALUES ($1, 'broadcast')`, [topic]);
        await c.query(`SET LOCAL ROLE ${role}`);
        await c.query(`SELECT set_config('request.jwt.claims', $1, true)`,
            [JSON.stringify(sub ? { sub, role, ...(email ? { email } : {}) } : { role })]);
        await c.query(`SELECT set_config('realtime.topic', $1, true)`, [topic]);
        return await fn(c);
    } finally {
        try { await c.query('ROLLBACK'); } catch { /* connection already broken */ }
        c.release();
    }
}

/** Would Realtime admit `sub` (signed in as `role`) to the private channel `topic`? */
const canReceive = (role: Role, sub: string | null, topic: string, email?: string) => asChannel(role, sub, topic, async (c) => {
    const { rows } = await c.query(
        `SELECT count(*)::int AS n FROM realtime.messages WHERE topic = $1 AND extension = 'broadcast'`, [topic]);
    return rows[0].n > 0;
}, email);

/** Would Realtime let `sub` SEND a broadcast on the private channel `topic`? */
const canSend = (role: Role, sub: string | null, topic: string, email?: string) => asChannel(role, sub, topic, async (c) => {
    try {
        await c.query('SAVEPOINT send');
        await c.query(`INSERT INTO realtime.messages(topic, extension) VALUES ($1, 'broadcast')`, [topic]);
        return true;
    } catch (e: any) {
        if (e?.code !== '42501') throw e; // only an RLS / privilege refusal is a "no"
        await c.query('ROLLBACK TO SAVEPOINT send');
        return false;
    }
}, email);

// Game ids are the first 6 hex digits of a UUID in production (createId), so they
// carry no hyphen; `hyphenGame` is a deliberately hyphenated one that starts with
// `game`'s id, to prove a topic is matched whole rather than by pieces.
const game = uuid().slice(0, 6);
const otherGame = uuid().slice(0, 6);
const hyphenGame = `${game}-x`;
const a = uuid(), b = uuid(), c = uuid(), stranger = uuid();

before(async () => {
    await applySchema();
    await pgPool.query(`
        -- Supabase's own definitions of the two functions the policies call.
        CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
          SELECT NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'sub', '')::uuid $$;
        CREATE OR REPLACE FUNCTION realtime.topic() RETURNS text LANGUAGE sql STABLE AS $$
          SELECT NULLIF(current_setting('realtime.topic', true), '') $$;
        -- Supabase's default privileges: client roles may read realtime.messages and
        -- public tables, and RLS is what narrows them.
        GRANT USAGE ON SCHEMA realtime TO anon, authenticated;
        GRANT SELECT, INSERT ON realtime.messages TO anon, authenticated;
        GRANT SELECT ON public.player_hands TO anon, authenticated;
        ALTER TABLE realtime.messages ENABLE ROW LEVEL SECURITY;
    `);
    // Kernel-owned lobby rows; seedTable writes the player_hands membership the policies read.
    await seedTable(game, fixture().seats([{ id: a, name: 'A' }, { id: b, name: 'B' }]).build());
    await seedTable(otherGame, fixture().seats([{ id: c, name: 'C' }]).build());
    await seedTable(hyphenGame, fixture().seats([{ id: c, name: 'C' }]).build());
    await pgPool.query(`INSERT INTO auth.users(id) VALUES ($1)`, [stranger]);
});

async function assertGuMatrix(stage: string): Promise<void> {
    const own = `gu-${game}-${a}`;
    const refused: [Role, string | null, string, string][] = [
        ['authenticated', b, own, 'another seated player on A\'s topic'],
        ['authenticated', stranger, own, 'a signed-in stranger on A\'s topic'],
        ['anon', null, own, 'anon on A\'s topic'],
        ['authenticated', stranger, `gu-${game}-${stranger}`, 'a stranger on a gu- topic naming themselves in a game they are not in'],
        ['authenticated', a, `gu-${otherGame}-${a}`, 'A on a game A is not in'],
        ['authenticated', a, `gu-${game}-${a.slice(0, 8)}`, 'A on a topic ending in only the first UUID group'],
        ['authenticated', a, `gu-${game}-${a}-x`, 'A on their topic with a suffix'],
        ['authenticated', a, `x-gu-${game}-${a}`, 'A on their topic with a prefix'],
        ['authenticated', a, `gu--${game}-${a}`, 'A on their topic with a doubled separator'],
        ['authenticated', a, `gu-${game}${a}`, 'A on their topic with no separator'],
        ['authenticated', a, `gu-${game}-`, 'A on a topic with no user'],
        ['authenticated', a, `gu-${a}`, 'A on a topic with no game'],
        ['authenticated', a, `GU-${game}-${a}`, 'A on an upper-cased prefix'],
        ['authenticated', a, `gu-${game}-${a.toUpperCase()}`, 'A on an upper-cased user id'],
        // A is in `game`; `game-x` is a different game A is not in, whose id merely
        // starts with `game`. Its topic for A must not ride on A's membership of `game`.
        ['authenticated', a, `gu-${hyphenGame}-${a}`, 'A on a hyphenated game whose id extends A\'s game id'],
    ];
    for (const [role, sub, topic, why] of refused) {
        assert.equal(await canReceive(role, sub, topic), false, `${stage}: refused: ${why} (${topic})`);
    }
    assert.equal(await canReceive('authenticated', a, own), true, `${stage}: A receives on A's own gu- topic (${own})`);
    assert.equal(await canReceive('authenticated', b, `gu-${game}-${b}`), true, `${stage}: B receives on B's own gu- topic`);
    assert.equal(await canReceive('authenticated', c, `gu-${hyphenGame}-${c}`), true,
        `${stage}: a hyphenated game id still authorizes its own member`);
}

async function assertChatMatrix(stage: string): Promise<void> {
    const topic = `chat:${game}`;
    assert.equal(await canReceive('authenticated', a, topic), true, `${stage}: a seated player receives the game's chat`);
    assert.equal(await canReceive('authenticated', b, topic), true, `${stage}: every seated player receives the game's chat`);
    const refused: [Role, string | null, string, string][] = [
        ['authenticated', stranger, topic, 'a signed-in stranger'],
        ['anon', null, topic, 'anon'],
        ['authenticated', c, topic, 'a player from another game'],
        ['authenticated', a, `chat:${game}:x`, 'a member on the chat topic with a suffix'],
        ['authenticated', a, `chat:${game}x`, 'a member on a longer game id'],
        ['authenticated', a, `chat:${otherGame}`, 'a member of one game on another game\'s chat'],
        ['authenticated', a, `xchat:${game}`, 'a member on the chat topic with a prefix'],
    ];
    for (const [role, sub, t, why] of refused) {
        assert.equal(await canReceive(role, sub, t), false, `${stage}: refused: ${why} (${t})`);
    }
}

async function assertGameMatrix(stage: string): Promise<void> {
    // Public by design: the fully-masked spectator stream.
    assert.equal(await canReceive('authenticated', stranger, `game-${game}`), true, `${stage}: a signed-in spectator receives game-`);
    assert.equal(await canReceive('anon', null, `game-${game}`), false, `${stage}: anon does not receive game-`);
}

// The retired user- family, and sending in general. `a` signs in as
// alice@a.com and `stranger` as alice@b.com: the same local part.
async function assertNoClientSendsAndNoUserTopics(stage: string): Promise<void> {
    assert.equal(await canReceive('authenticated', a, 'user-alice', 'alice@a.com'), false,
        `${stage}: nobody receives on a user- topic, not even the address it was named after`);
    assert.equal(await canReceive('authenticated', stranger, 'user-alice', 'alice@b.com'), false,
        `${stage}: another address with the same local part does not receive it`);
    const topics = ['user-alice', `gu-${game}-${a}`, `chat:${game}`, `game-${game}`];
    for (const topic of topics) {
        assert.equal(await canSend('authenticated', a, topic, 'alice@a.com'), false, `${stage}: a signed-in user cannot send on ${topic}`);
        assert.equal(await canSend('anon', null, topic), false, `${stage}: anon cannot send on ${topic}`);
    }
}

test('user- and sending: no client receives a user- topic or sends on any topic', async () => {
    await assertNoClientSendsAndNoUserTopics('seed.sql');
});

test('gu-: a seated player receives their own animation stream, and only that', async () => {
    await assertGuMatrix('seed.sql');
});

test('chat:: seated players receive their game\'s chat, on the exact topic only', async () => {
    await assertChatMatrix('seed.sql');
});

test('game-: any signed-in user receives the spectator stream, anon does not', async () => {
    await assertGameMatrix('seed.sql');
});

// Hosted receives migrations, never seed.sql, and its realtime policies were
// created from seed.sql as it stood before these migrations. So put that legacy
// set back, apply the migrations in order, and require exactly the policies a
// fresh seed.sql database has - then the whole matrix again.
//
// A FROZEN FIXTURE, copied verbatim from seed.sql at a844b2a1. It is what the
// migrations must be able to start from; it is never to be "fixed".
const LEGACY_POLICIES = `
DROP POLICY IF EXISTS "authenticated can receive game-user messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive chat broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";
CREATE POLICY "authenticated can receive private messages" ON "realtime"."messages" FOR SELECT TO authenticated USING (
  (SELECT realtime.topic()) = CONCAT('user-', split_part(((select current_setting('request.jwt.claims', true))::jsonb ->> 'email'), '@', 1))
  AND realtime.messages.extension IN ('broadcast'));
CREATE POLICY "authenticated can send private messages" ON "realtime"."messages" FOR INSERT TO authenticated WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%' AND realtime.messages.extension IN ('broadcast'));
CREATE POLICY "authenticated can receive game-user messages" ON "realtime"."messages" FOR SELECT TO authenticated USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  split_part((SELECT realtime.topic()), '-', 3) = (select auth.uid())::text AND
  EXISTS (SELECT 1 FROM player_hands WHERE player_id = (select auth.uid()) AND game_id = split_part((SELECT realtime.topic()), '-', 2)) AND
  realtime.messages.extension IN ('broadcast'));
CREATE POLICY "authenticated can receive chat broadcasts" ON "realtime"."messages" FOR SELECT TO authenticated USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  EXISTS (SELECT 1 FROM player_hands WHERE player_id = (select auth.uid()) AND game_id = split_part((SELECT realtime.topic()), ':', 2)) AND
  realtime.messages.extension IN ('broadcast'));
CREATE POLICY "service role can send private messages" ON "realtime"."messages" FOR INSERT TO service_role WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%' AND realtime.messages.extension IN ('broadcast'));
-- AND the one hosted carried that this repo never wrote: a blanket SELECT, read
-- off the live catalog on 2026-09-18. With the legacy gu- policy refusing
-- everyone, THIS is what admitted every join on hosted - including one player to
-- another player's per-seat stream.
CREATE POLICY "tmp_allow_all" ON "realtime"."messages" FOR SELECT TO authenticated USING (true);
`;

/** No policy may admit a topic unconditionally: that is what tmp_allow_all did. */
async function assertNoBlanketPolicy(stage: string): Promise<void> {
    const { rows } = await pgPool.query(
        `SELECT policyname, qual FROM pg_policies
         WHERE schemaname = 'realtime' AND tablename = 'messages'
           AND cmd = 'SELECT' AND coalesce(btrim(qual), '') IN ('true', '(true)')`);
    assert.deepEqual(rows, [], `${stage}: a SELECT policy on realtime.messages admits every topic: ${rows.map((r) => r.policyname).join(', ')}`);
}

test('the migrations take the legacy hosted policies to exactly the seed.sql set', async () => {
    for (const [i, m] of REALTIME_MIGRATIONS.entries()) assert.ok(m, `realtime migration ${i + 1} exists`);
    const policies = async () => (await pgPool.query(
        `SELECT policyname, cmd, roles::text, qual, with_check FROM pg_policies
         WHERE schemaname = 'realtime' AND tablename = 'messages' ORDER BY policyname`)).rows;
    const fromSeed = await policies();

    await assertNoBlanketPolicy('seed.sql');

    await pgPool.query(LEGACY_POLICIES);
    // Fixture sanity, both halves of what hosted actually looked like: the legacy
    // gu- policy refuses its owner, and tmp_allow_all admits a stranger to it.
    assert.equal(await canReceive('authenticated', b, `gu-${game}-${a}`), true,
        'fixture sanity: tmp_allow_all admits another player to A\'s per-seat stream');

    for (const m of REALTIME_MIGRATIONS) await pgPool.query(readFileSync(join(MIGRATIONS, m!), 'utf8'));
    assert.deepEqual(await policies(), fromSeed);
    await assertNoBlanketPolicy('migration');
    await assertGuMatrix('migration');
    await assertChatMatrix('migration');
    await assertGameMatrix('migration');
    await assertNoClientSendsAndNoUserTopics('migration');
});
