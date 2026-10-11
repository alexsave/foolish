// auth_pinned_keys.test.ts - the keys a token is verified against are PINNED in
// the function's configuration (JWT_VERIFY_KEYS), not fetched.
//
// auth.ts used to GET /auth/v1/.well-known/jwks.json and parse it as JSON. The
// keys are configuration now: <kid>:<alg>:<base64 SPKI>, comma separated, or the
// single word `gotrue`. This holds what that buys and what it must never do:
//
//   - a token signed by a pinned key verifies, with NO network request at all;
//   - a missing or unreadable configuration refuses EVERY request, loudly, and
//     never reaches the GoTrue fallback (a function that does not know which
//     keys it trusts must not quietly ask somebody else instead);
//   - a kid that is not pinned is not accepted locally (a rotation in progress
//     goes to GoTrue, with a warning naming the kid);
//   - a key pinned for one algorithm does not verify a token claiming the other.
//
// Pure Web Crypto, no Postgres. The envelope cases (tamper, expiry, the alg
// confusions) are e2e/auth_jwt.test.ts.
import { test, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { getAuthenticatedUser, verifyJwtLocal, parseVerifyKeys, AuthNotConfigured, __setJwksForTest }
    from '../server/impls/supabase/functions/_shared/adapter/auth.ts';

// The configuration this file controls. auth.ts reads Deno.env when a token
// arrives, never at load, so this is in place before anything asks.
const env: Record<string, string | undefined> = { SUPABASE_URL: 'http://gotrue.test', SUPABASE_SERVICE_ROLE_KEY: 'service' };
(globalThis as any).Deno = { env: { get: (k: string) => env[k] } };

const enc = new TextEncoder();
const b64 = (bytes: Uint8Array): string => Buffer.from(bytes).toString('base64');
const b64url = (bytes: Uint8Array): string => Buffer.from(bytes).toString('base64url');

type Alg = 'ES256' | 'RS256';
const GEN: Record<Alg, EcKeyGenParams | RsaHashedKeyGenParams> = {
    ES256: { name: 'ECDSA', namedCurve: 'P-256' },
    RS256: { name: 'RSASSA-PKCS1-v1_5', modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: 'SHA-256' },
};
const SIGN: Record<Alg, EcdsaParams | AlgorithmIdentifier> = {
    ES256: { name: 'ECDSA', hash: 'SHA-256' }, RS256: { name: 'RSASSA-PKCS1-v1_5' },
};

/** A key pair and its public half as the configuration states it. */
async function key(alg: Alg, kid: string) {
    const kp = await crypto.subtle.generateKey(GEN[alg], true, ['sign', 'verify']) as CryptoKeyPair;
    const spki = new Uint8Array(await crypto.subtle.exportKey('spki', kp.publicKey));
    return { kp, kid, alg, pin: `${kid}:${alg}:${b64(spki)}` };
}

// 2100-01-01, a constant: no case here is about expiry, so none reads the clock to make a token.
const FAR = 4102444800;

async function mint(k: { kp: CryptoKeyPair; alg: Alg }, kid: string, sub: string, headerAlg: string = k.alg): Promise<string> {
    const h = b64url(enc.encode(JSON.stringify({ alg: headerAlg, kid, typ: 'JWT' })));
    const p = b64url(enc.encode(JSON.stringify({ sub, aud: 'authenticated', exp: FAR, user_metadata: { username: 'ana' } })));
    const sig = new Uint8Array(await crypto.subtle.sign(SIGN[k.alg], k.kp.privateKey, enc.encode(`${h}.${p}`)));
    return `${h}.${p}.${b64url(sig)}`;
}

const request = (token: string) => new Request('http://edge.local/functions/v1/meta', { method: 'POST', headers: { Authorization: `Bearer ${token}` } });

// Every network request auth itself makes, recorded and refused. There are none
// left: the JWKS fetch was the only one. (The GoTrue fallback goes through
// supabase-js, which this lane stands in for, so a fallback shows here as the
// fallback's own refusal, "Invalid token", and never as AuthNotConfigured.)
const fetched: string[] = [];
const realFetch = globalThis.fetch;
globalThis.fetch = (async (input: unknown) => {
    fetched.push(typeof input === 'string' ? input : String((input as { url?: string })?.url ?? input));
    return new Response(null, { status: 401 });
}) as typeof fetch;

const errors: string[] = [], warnings: string[] = [];
const realError = console.error, realWarn = console.warn, realLog = console.log;
console.error = (...a: unknown[]) => { errors.push(a.map(String).join(' ')); };
console.warn = (...a: unknown[]) => { warnings.push(a.map(String).join(' ')); };
console.log = () => {};

beforeEach(() => { fetched.length = 0; errors.length = 0; warnings.length = 0; __setJwksForTest(null); });
after(() => { globalThis.fetch = realFetch; console.error = realError; console.warn = realWarn; console.log = realLog; });

test('a token signed by a pinned key verifies locally, with no network request', async () => {
    const ec = await key('ES256', 'kid-ec'), rs = await key('RS256', 'kid-rs');
    env.JWT_VERIFY_KEYS = ` ${ec.pin} , ${rs.pin} `;
    const a = await getAuthenticatedUser(request(await mint(ec, 'kid-ec', 'user-ec')));
    assert.equal(a.id, 'user-ec');
    assert.equal(a.user_metadata.username, 'ana', 'the user the server reads is the token\'s claims');
    const b = await getAuthenticatedUser(request(await mint(rs, 'kid-rs', 'user-rs')));
    assert.equal(b.id, 'user-rs', 'two keys pinned at once: a rotation\'s standby beside the current one');
    assert.deepEqual(fetched, [], 'no JWKS is fetched and GoTrue is not asked');
    assert.deepEqual(errors, []);
});

test('no configuration refuses every request, loudly, and never falls back to GoTrue', async () => {
    const ec = await key('ES256', 'kid-ec');
    const token = await mint(ec, 'kid-ec', 'user-ec');
    for (const [value, why] of [
        [undefined, /is not set/], ['', /is not set/], ['   ', /is not set/],
        ['x', /not <kid>:<alg>:<base64 SPKI>/],
        [`kid-ec:HS256:${ec.pin.split(':')[2]}`, /not ES256 or RS256/],
        ['kid-ec:ES256:***', /not base64/],
        [`:ES256:${ec.pin.split(':')[2]}`, /no kid/],
        [`${ec.pin},${ec.pin}`, /twice/],
        [`${ec.pin},`, /not <kid>:<alg>:<base64 SPKI>/],
    ] as const) {
        env.JWT_VERIFY_KEYS = value;
        errors.length = 0;
        await assert.rejects(getAuthenticatedUser(request(token)), (e: Error) => e instanceof AuthNotConfigured && why.test(e.message),
            `JWT_VERIFY_KEYS=${JSON.stringify(value)} refuses a token a pinned key would have verified`);
        assert.ok(errors.some((l) => /JWT_VERIFY_KEYS/.test(l) && /Every request is refused/.test(l)), `and says so in the log (${JSON.stringify(value)})`);
        assert.equal(await verifyJwtLocal(token), null, 'nothing verifies locally either');
    }
    assert.deepEqual(fetched, [], 'and nothing was fetched: an unconfigured server judges nobody');
});

test('a kid that is not pinned is not accepted locally: it goes to GoTrue, and the log names it', async () => {
    const pinnedKey = await key('ES256', 'kid-current'), rotated = await key('ES256', 'kid-next');
    env.JWT_VERIFY_KEYS = pinnedKey.pin;
    const token = await mint(rotated, 'kid-next', 'user-x');
    assert.equal(await verifyJwtLocal(token), null, 'a correctly signed token under an unpinned kid does not verify here');
    await assert.rejects(getAuthenticatedUser(request(token)), /Invalid token/, 'and GoTrue (which refuses it here) has the last word');
    assert.deepEqual(fetched, [], 'and no key set was fetched to find out');
    assert.ok(warnings.some((l) => /kid-next/.test(l) && /does not pin/.test(l)), 'the warning names the kid to pin');

    // The same kid name under ANOTHER key: pinning is of the key, not the name.
    const imposter = await key('ES256', 'kid-current');
    assert.equal(await verifyJwtLocal(await mint(imposter, 'kid-current', 'user-x')), null, 'a different key claiming a pinned kid does not verify');
});

test('a key is pinned WITH its algorithm: a token claiming the other one does not verify', async () => {
    const ec = await key('ES256', 'kid-ec'), rs = await key('RS256', 'kid-rs');
    env.JWT_VERIFY_KEYS = `${ec.pin},${rs.pin}`;
    assert.ok(await verifyJwtLocal(await mint(ec, 'kid-ec', 'u')), 'control: the honest token verifies');
    assert.equal(await verifyJwtLocal(await mint(ec, 'kid-ec', 'u', 'RS256')), null, 'an EC key under an RS256 header');
    assert.equal(await verifyJwtLocal(await mint(rs, 'kid-rs', 'u', 'ES256')), null, 'an RSA key under an ES256 header');
    // A key whose bytes are not the algorithm it is pinned as never imports.
    env.JWT_VERIFY_KEYS = `kid-ec:RS256:${ec.pin.split(':')[2]}`;
    assert.equal(await verifyJwtLocal(await mint(ec, 'kid-ec', 'u')), null, 'pinned as RS256, the EC key verifies nothing');
});

test('`gotrue` pins nothing on purpose: every token is verified at GoTrue', async () => {
    const ec = await key('ES256', 'kid-ec');
    env.JWT_VERIFY_KEYS = 'gotrue';
    const token = await mint(ec, 'kid-ec', 'user-ec');
    assert.equal(await verifyJwtLocal(token), null, 'nothing is verified locally');
    await assert.rejects(getAuthenticatedUser(request(token)), /Invalid token/);
    assert.deepEqual(errors.filter((l) => /JWT_VERIFY_KEYS/.test(l)), [], 'and this is a stated choice, not an error');
    assert.ok(warnings.some((l) => /gotrue/.test(l)), 'said once in the log');
});

test('the configuration reads as a key list, and nothing in it is JSON', () => {
    assert.equal(parseVerifyKeys('gotrue'), null);
    const keys = parseVerifyKeys('a:ES256:AQID,b:RS256:BAUG');
    assert.deepEqual([...keys!.keys()], ['a', 'b']);
    assert.deepEqual(keys!.get('a'), { alg: 'ES256', spki: Uint8Array.of(1, 2, 3) });
    assert.throws(() => parseVerifyKeys('{"keys":[{"kid":"a","kty":"EC"}]}'), AuthNotConfigured, 'a JWKS pasted in is refused, not parsed');
});
