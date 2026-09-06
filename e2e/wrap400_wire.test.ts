// wrap400's RESPONSE WIRE - the generic tail every `meta` request and the
// `action` bump nudge come back through.
//
// That tail used to be `new Response(JSON.stringify(personalViewOf(...)))`: a
// whole personalized game, masked by the kernel and then re-serialized as JSON,
// on the one edge response that had not been cut over. It now returns the PACKED
// envelope - the same bytes `create` returns, `player_views.view` stores and the
// realtime feed pushes.
//
// Nothing tested it. Every e2e suite reaches the server through
// executeWithGameLock / handleMetaAction, i.e. BELOW wrap400; the handler itself
// was only ever exercised in production (auth_jwt.test.ts's header says as much:
// "the HTTP handler using it isn't integration-tested"). So a JSON body could
// have come back forever with every suite green. This test calls the real
// handler wrap400 returns, over a real Request, with a real signed token.
//
// No Postgres and no network: the request carries NO game_id, which is
// wrap400's own "operations that don't involve games" branch - it runs the
// supplied execute directly and then shapes the response, which is precisely
// the code under test. The JWKS is injected (as in auth_jwt.test.ts) so the
// auth step is real signature verification, offline.

import './harness.ts';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { wrap400 } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { __setJwksForTest } from '../server/impls/supabase/functions/_shared/adapter/auth.ts';
import { personalViewOf } from '../server/api/common/player_views.ts';
import { decodePackedGame, GAME_RESP_FORMAT } from '../sdk/ts/wire/view.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import {
    Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, PersonalGame, PublicGame, PrivatePlayer,
} from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

__setKernelSeedSource(() => 4242); // pin the deal so the fixture is reproducible

// ---- a signed token, minted independently of the verifier -------------------

const enc = new TextEncoder();
const b64url = (bytes: Uint8Array): string => {
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
};
const b64urlStr = (s: string): string => b64url(enc.encode(s));

async function mintToken(sub: string, username: string): Promise<string> {
    const kp = await crypto.subtle.generateKey(
        { name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
    const jwk = await crypto.subtle.exportKey('jwk', kp.publicKey) as JsonWebKey & { kid?: string; use?: string };
    jwk.kid = 'wrap400'; jwk.use = 'sig';
    __setJwksForTest({ keys: [jwk] });

    // A FIXED far-future expiry rather than now+1h: expiry is auth_jwt.test.ts's
    // subject, not this file's, and reading the clock here would be entropy the
    // determinism gate is right to refuse.
    const EXP_2100 = 4102444800;
    const h = b64urlStr(JSON.stringify({ alg: 'ES256', kid: 'wrap400', typ: 'JWT' }));
    const p = b64urlStr(JSON.stringify({
        sub, aud: 'authenticated', role: 'authenticated',
        exp: EXP_2100, user_metadata: { username },
    }));
    const sig = new Uint8Array(await crypto.subtle.sign(
        { name: 'ECDSA', hash: 'SHA-256' }, kp.privateKey, enc.encode(`${h}.${p}`)));
    return `${h}.${p}.${b64url(sig)}`;
}

// ---- fixtures ---------------------------------------------------------------

const HUMAN_A = 'aaaaaaaa-0000-4000-8000-000000000001';
const HUMAN_B = 'bbbbbbbb-0000-4000-8000-000000000002';
const BOT = 'cccccccc-0000-4000-8000-000000000003';
const OUTSIDER = 'dddddddd-0000-4000-8000-000000000004';

const mkPlayer = (id: string, name: string, isAi: boolean): PrivatePlayer => ({
    player_id: id, name, status: PLAYER_STATUS.READY, is_ai: isAi,
    hand: [], hand_length: 0, awaiting_attack: false,
    strategy_key: isAi ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.HUMAN,
});

const mkLobby = (): Game => ({
    id: 'wrap400game', name: 'wrap400 wire', status: GAME_STATUS.WAITING,
    players: [mkPlayer(HUMAN_A, 'A', false), mkPlayer(HUMAN_B, 'B', false), mkPlayer(BOT, 'Bot', true)],
    deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
    power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
    elimination_order: [], good_timestamp: null, good_players: [], logs: [],
    version: 42,
});

const dealt = (): Game => {
    const g = mkLobby();
    start_game(g);
    g.status = GAME_STATUS.PLAYING;
    return g;
};

/** Drive one request through the REAL wrap400 handler. */
async function call(game: Game, sub: string, username: string): Promise<Response> {
    const token = await mintToken(sub, username);
    // No `binary` escape hatch and no game_id: this is exactly the branch a
    // `meta` request (and the `action` bump) takes.
    const handler = wrap400(async () => ({ game, events: [] }));
    return handler(new Request('http://local/meta', {
        method: 'POST',
        headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'start' }),
    }));
}

// ---- the wire ---------------------------------------------------------------

test('wrap400 answers with the PACKED game envelope, never a JSON game', async () => {
    for (const [tag, game] of [['lobby', mkLobby()], ['dealt', dealt()]] as const) {
        const res = await call(game, HUMAN_A, 'A');
        assert.equal(res.status, 200, `${tag}: 200`);
        assert.equal(res.headers.get('Content-Type'), 'application/octet-stream', `${tag}: octet-stream`);

        const bytes = new Uint8Array(await res.arrayBuffer());
        assert.equal(bytes[0], GAME_RESP_FORMAT, `${tag}: leads with the packed envelope format byte`);

        // The body is BYTES, not a JSON document. This is the assertion that
        // fails the moment the tail goes back to JSON.stringify.
        assert.throws(
            () => JSON.parse(new TextDecoder().decode(bytes)),
            `${tag}: body must not parse as JSON`,
        );

        // And it carries exactly what the JSON body carried: the same game the
        // retired `personalViewOf` response produced, for the same caller.
        const decoded = decodePackedGame(bytes);
        assert.ok(decoded, `${tag}: decodePackedGame reads it`);
        assert.equal(decoded!.version, 42, `${tag}: the row's version rides the envelope`);
        assert.equal(decoded!.seat, 0, `${tag}: the caller's seat`);
        assert.deepEqual(
            decoded!.game,
            await personalViewOf(game, HUMAN_A) as PersonalGame,
            `${tag}: decodes to what the JSON body used to be`,
        );
    }
});

test('wrap400: a caller with no seat gets the spectator envelope', async () => {
    const game = dealt();
    const res = await call(game, OUTSIDER, 'Nobody');
    assert.equal(res.headers.get('Content-Type'), 'application/octet-stream');

    const bytes = new Uint8Array(await res.arrayBuffer());
    const decoded = decodePackedGame(bytes);
    assert.ok(decoded, 'spectator envelope decodes');
    assert.equal(decoded!.seat, -1, 'seat -1');
    assert.equal((decoded!.game as PersonalGame).self, undefined, 'a spectator gets no self');
    assert.deepEqual(
        decoded!.game,
        await personalViewOf(game, OUTSIDER) as PublicGame,
        'the spectator view matches the retired JSON one',
    );
    // Masking is still the kernel's: no hand identities for anyone.
    for (const p of decoded!.game.players) {
        assert.ok(p.hand_length > 0, 'hand counts are real');
    }
});

test('wrap400: seat 1 sees its OWN hand', async () => {
    const game = dealt();
    const res = await call(game, HUMAN_B, 'B');
    const decoded = decodePackedGame(new Uint8Array(await res.arrayBuffer()));
    assert.ok(decoded);
    assert.equal(decoded!.seat, 1);
    const self = (decoded!.game as PersonalGame).self;
    assert.ok(self, 'seat 1 gets a self');
    assert.deepEqual(
        self!.hand, game.players[1].hand.map(c => ({ suit: c.suit, value: c.value })),
        'the viewer\'s own hand is real',
    );
    assert.equal(self!.player_id, HUMAN_B);
});
