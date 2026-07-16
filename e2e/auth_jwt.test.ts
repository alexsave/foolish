// Local JWT verification (server/impls/supabase/functions/_shared/auth.ts) — the security
// boundary that replaced supabase-js `auth.getClaims` to keep supabase-js out of
// the edge-function boot graph. The cryptographic check is the platform's
// crypto.subtle.verify; what auth.ts owns is the JWS envelope parsing + a hard
// algorithm pin. Because that plumbing is ours (and the HTTP handler using it
// isn't integration-tested), this proves it holds: valid ES256 AND RS256 tokens
// verify, and every way a token can be forged or staled is refused — tamper,
// expiry, not-yet-valid, unknown key, wrong signer, and the two shapes of the
// JWT alg-confusion attack. Pure Web Crypto; no Postgres, no network (JWKS
// injected). Tokens are signed here with crypto.subtle independently of the
// verifier, so a parsing/format bug can't pass by construction.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { verifyJwtLocal, __setJwksForTest, unverifiedSubFromToken } from '../server/impls/supabase/functions/_shared/adapter/auth.ts';

const enc = new TextEncoder();
const b64url = (bytes: Uint8Array): string => {
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
};
const b64urlStr = (s: string): string => b64url(enc.encode(s));

type Alg = 'ES256' | 'RS256';
const SIGN_PARAMS: Record<Alg, EcdsaParams | AlgorithmIdentifier> = {
    ES256: { name: 'ECDSA', hash: 'SHA-256' },
    RS256: { name: 'RSASSA-PKCS1-v1_5' },
};
const GEN_PARAMS: Record<Alg, EcKeyGenParams | RsaHashedKeyGenParams> = {
    ES256: { name: 'ECDSA', namedCurve: 'P-256' },
    RS256: { name: 'RSASSA-PKCS1-v1_5', modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: 'SHA-256' },
};

async function genKey(alg: Alg, kid: string) {
    const kp = await crypto.subtle.generateKey(GEN_PARAMS[alg], true, ['sign', 'verify']);
    const jwk = await crypto.subtle.exportKey('jwk', kp.publicKey) as JsonWebKey & { kid?: string; use?: string };
    // Deliberately DO NOT set jwk.alg. `alg` is optional in a JWK (RFC 7517) and
    // Supabase's JWKS omits it; the verifier must derive the algorithm from the
    // token header, not the JWK. (An earlier version required jwk.alg and so
    // rejected every Supabase token, silently falling back to the slow getUser.)
    jwk.kid = kid; jwk.use = 'sig';
    return { kp, jwk };
}

// Sign header.payload with `signKey` under `headerAlg`/`headerKid`. Kept flexible
// so the header can be made to lie about the algorithm/key (alg-confusion tests).
async function mint(signKey: CryptoKey, signAlg: Alg, headerAlg: string, headerKid: string, payload: object): Promise<string> {
    const h = b64urlStr(JSON.stringify({ alg: headerAlg, kid: headerKid, typ: 'JWT' }));
    const p = b64urlStr(JSON.stringify(payload));
    const sig = new Uint8Array(await crypto.subtle.sign(SIGN_PARAMS[signAlg], signKey, enc.encode(`${h}.${p}`)));
    return `${h}.${p}.${b64url(sig)}`;
}

test('verifyJwtLocal: ES256 & RS256 verify; tamper/expiry/wrong-signer/alg-confusion refused', async () => {
    const ec = await genKey('ES256', 'ec1');
    const rs = await genKey('RS256', 'rs1');
    const other = await genKey('ES256', 'ec1'); // DIFFERENT key claiming the same kid → wrong signer
    __setJwksForTest({ keys: [ec.jwk, rs.jwk] });

    try {
        const now = Math.floor(Date.now() / 1000);
        const base = { aud: 'authenticated', role: 'authenticated', exp: now + 3600, iat: now, user_metadata: { username: 'alice' } };

        // 1. Valid ES256 → verifies, claims intact.
        const es = await verifyJwtLocal(await mint(ec.kp.privateKey, 'ES256', 'ES256', 'ec1', { ...base, sub: 'u-ec' }));
        assert.ok(es, 'valid ES256 verifies');
        assert.equal(es!.sub, 'u-ec');
        assert.equal((es!.user_metadata as { username?: string })?.username, 'alice');

        // 2. Valid RS256 → verifies (proves the RSA branch + its alg pin work).
        const rsClaims = await verifyJwtLocal(await mint(rs.kp.privateKey, 'RS256', 'RS256', 'rs1', { ...base, sub: 'u-rs' }));
        assert.ok(rsClaims, 'valid RS256 verifies');
        assert.equal(rsClaims!.sub, 'u-rs');

        // 3. Tampered payload reusing a real signature → refused (both algs).
        for (const [k, alg, kid] of [[ec.kp.privateKey, 'ES256', 'ec1'], [rs.kp.privateKey, 'RS256', 'rs1']] as const) {
            const good = await mint(k, alg, alg, kid, { ...base, sub: 'real' });
            const [h, , s] = good.split('.');
            const forged = `${h}.${b64urlStr(JSON.stringify({ ...base, sub: 'attacker' }))}.${s}`;
            assert.equal(await verifyJwtLocal(forged), null, `tampered ${alg} payload refused`);
        }

        // 4. Expired / not-yet-valid → refused.
        assert.equal(await verifyJwtLocal(await mint(ec.kp.privateKey, 'ES256', 'ES256', 'ec1', { ...base, sub: 'x', exp: now - 3600 })), null, 'expired refused');
        assert.equal(await verifyJwtLocal(await mint(ec.kp.privateKey, 'ES256', 'ES256', 'ec1', { ...base, sub: 'x', nbf: now + 3600 })), null, 'nbf-future refused');

        // 5. Unknown kid → no key → refused.
        assert.equal(await verifyJwtLocal(await mint(ec.kp.privateKey, 'ES256', 'ES256', 'nope', { ...base, sub: 'x' })), null, 'unknown kid refused');

        // 6. Wrong signer: valid structure, correct kid, but signed by a key that
        //    is NOT the one in the JWKS → signature check fails.
        assert.equal(await verifyJwtLocal(await mint(other.kp.privateKey, 'ES256', 'ES256', 'ec1', { ...base, sub: 'x' })), null, 'wrong signer refused');

        // 7. Alg-confusion (symmetric): a token DECLARING HS256/none must be
        //    refused outright, never checked against the public key.
        const p = b64urlStr(JSON.stringify({ ...base, sub: 'x' }));
        const hsHeader = b64urlStr(JSON.stringify({ alg: 'HS256', kid: 'ec1', typ: 'JWT' }));
        assert.equal(await verifyJwtLocal(`${hsHeader}.${p}.${b64url(enc.encode('forged'))}`), null, 'HS256 alg refused');
        const noneHeader = b64urlStr(JSON.stringify({ alg: 'none', kid: 'ec1', typ: 'JWT' }));
        assert.equal(await verifyJwtLocal(`${noneHeader}.${p}.`), null, 'alg=none refused');

        // 8. Alg-substitution (asymmetric): header claims RS256 but the kid maps
        //    to the EC key (signed as ES256). The header-alg/key-alg bind rejects it.
        assert.equal(await verifyJwtLocal(await mint(ec.kp.privateKey, 'ES256', 'RS256', 'ec1', { ...base, sub: 'x' })), null, 'ES-key-as-RS256 refused');
        assert.equal(await verifyJwtLocal(await mint(rs.kp.privateKey, 'RS256', 'ES256', 'rs1', { ...base, sub: 'x' })), null, 'RS-key-as-ES256 refused');

        // 9. Structurally malformed → refused, never throws.
        for (const bad of ['not-a-jwt', 'a.b', '', 'a.b.c.d', '..', 'x.y.z']) {
            assert.equal(await verifyJwtLocal(bad), null, `malformed (${JSON.stringify(bad)}) refused`);
        }
    } finally {
        __setJwksForTest(null);
    }
});

test('unverifiedSubFromToken decodes sub WITHOUT verifying (latency helper only)', () => {
    const enc2 = new TextEncoder();
    const b = (s: string) => b64url(enc2.encode(s));
    const hdr = b(JSON.stringify({ alg: 'ES256', kid: 'k', typ: 'JWT' }));

    // Well-formed token with a GARBAGE signature: sub is still returned. This is
    // the whole point — the caller fires subject-scoped work in parallel and MUST
    // still await verifyJwtLocal before returning anything. If this ever started
    // "verifying", the parallel optimization would be pointless.
    const tok = `${hdr}.${b(JSON.stringify({ sub: 'user-xyz' }))}.${b('not-a-real-signature')}`;
    assert.equal(unverifiedSubFromToken(tok), 'user-xyz');

    // No sub / malformed / wrong shape → null (caller then rejects).
    assert.equal(unverifiedSubFromToken(`${hdr}.${b(JSON.stringify({ foo: 1 }))}.${b('x')}`), null, 'no sub → null');
    assert.equal(unverifiedSubFromToken(`${hdr}.${b(JSON.stringify({ sub: '' }))}.${b('x')}`), null, 'empty sub → null');
    assert.equal(unverifiedSubFromToken('a.b'), null, 'two-part → null');
    assert.equal(unverifiedSubFromToken('not-a-jwt'), null, 'garbage → null');
    assert.equal(unverifiedSubFromToken(''), null, 'empty → null');
});
