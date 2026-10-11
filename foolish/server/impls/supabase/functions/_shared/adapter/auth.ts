import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { ASK_WHY_AUTH } from '@sdk/ts/gen/game_layout.bots.ts';
import type { User } from 'jsr:@supabase/supabase-js'; // type-only → erased at compile time, no runtime/boot cost

/**
 * Authenticates a user from the request's Authorization header.
 *
 * Fast path — LOCAL asymmetric verification (verifyJwtLocal below). With
 * asymmetric JWT signing keys (ES256/RS256) enabled on the project, the token is
 * verified against the project's public keys, PINNED in this function's
 * configuration (JWT_VERIFY_KEYS, below), right here: no GoTrue round-trip
 * and — the reason this was rewritten — NO `@supabase/supabase-js` in the boot
 * graph. supabase-js was the single heaviest module every edge function
 * evaluated on a cold isolate; auth runs on every request, so lazy-importing it
 * wouldn't have helped (a cold request pays it regardless). Verifying locally
 * with the platform's Web Crypto (crypto.subtle) removes it from the cold path
 * entirely.
 *
 * We do NOT implement any cryptography here: the ECDSA/RSA signature check IS
 * `crypto.subtle.verify` (the platform's validated crypto). What this module
 * owns is only the JWT envelope parsing and a hard algorithm pin — see
 * verifyJwtLocal — using the PUBLIC key only, so it can't forge tokens.
 *
 * Fallback — supabase-js `auth.getUser(token)`: if the project still uses the
 * legacy HS256 shared secret (JWT_VERIFY_KEYS says `gotrue`), the token names a
 * key that is not pinned (a rotation in progress), or local verification fails
 * for any reason, fall back to server-side verification so auth never silently
 * breaks. supabase-js is imported LAZILY here, so the common (asymmetric) case
 * never loads it.
 *
 * NO CONFIGURATION IS NOT A FALLBACK. With JWT_VERIFY_KEYS unset or unreadable
 * every request is refused (AuthNotConfigured, logged as an error): a function
 * that does not know which keys it trusts must not quietly settle for asking
 * somebody else on every request.
 *
 * Downstream only reads `user.id` and `user.user_metadata.username`.
 *
 * @param req The incoming request object
 * @returns The authenticated user object
 * @throws Error if the authorization header is missing or the token is invalid
 */
export async function getAuthenticatedUser(req: Request): Promise<User> {
    const authHeader = req.headers.get('Authorization');
    if (!authHeader) {
        throw Object.assign(new Error('No authorization header'), { why: ASK_WHY_AUTH });
    }

    const token = authHeader.replace('Bearer ', '');

    // Which keys are trusted is configuration, read before any token is: a
    // missing or unreadable JWT_VERIFY_KEYS throws here, loudly, for everyone.
    if (!injectedJwks) pinnedKeys();

    // Fast path: local signature verification against the pinned keys.
    const tAuth = performance.now();
    const claims = await verifyJwtLocal(token);
    if (claims) {
        console.log(`[perf] auth local-verify ${(performance.now() - tAuth).toFixed(0)}ms`);
        return claimsToUser(claims);
    }

    // Authoritative fallback: server-side verification at GoTrue. Only reached
    // for legacy HS256 projects or a token we couldn't verify locally — so
    // supabase-js loads here, off the common cold path, not at module scope.
    console.warn('[perf] auth local-verify MISS → supabase-js getUser fallback (slow)');
    try {
        const { createClient } = await import('jsr:@supabase/supabase-js');
        const client = createClient(
            Deno.env.get('SUPABASE_URL') || '',
            Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '',
        );
        const { data: { user }, error } = await client.auth.getUser(token);
        console.log(`[perf] auth getUser fallback ${(performance.now() - tAuth).toFixed(0)}ms`);
        if (error) {
            console.error('Authentication error:', error.message);
            throw new Error(`Invalid token: ${error.message}`);
        }
        if (!user) {
            throw new Error('User not found');
        }
        return user;
    } catch (e) {
        throw Object.assign(new Error(`Invalid token: ${(e as Error).message}`), { why: ASK_WHY_AUTH });
    }
}

// ============================================================================
// Local JWT verification. The cryptography is the platform's Web Crypto; this
// only parses the JWS envelope and pins the algorithm. No supabase-js.
// ============================================================================

// The ONLY algorithms accepted — both ASYMMETRIC, verified with a PUBLIC key.
// This is the alg-confusion defense: HS256/HS384/HS512 (symmetric) and "none"
// are absent, so we never treat a public key as an HMAC secret, and we
// only ever import keys / call verify as ECDSA or RSA. A token declaring any
// other alg is rejected before any crypto runs.
type Alg = 'ES256' | 'RS256';
const isAlg = (a: unknown): a is Alg => a === 'ES256' || a === 'RS256';
const IMPORT_PARAMS: Record<Alg, EcKeyImportParams | RsaHashedImportParams> = {
    ES256: { name: 'ECDSA', namedCurve: 'P-256' },
    RS256: { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' },
};
const VERIFY_PARAMS: Record<Alg, EcdsaParams | AlgorithmIdentifier> = {
    ES256: { name: 'ECDSA', hash: 'SHA-256' }, // JWS ES256 signature is raw r||s — exactly what crypto.subtle.verify expects
    RS256: { name: 'RSASSA-PKCS1-v1_5' },
};

// ============================================================================
// The pinned keys: JWT_VERIFY_KEYS.
//
// The project's public signing keys used to be FETCHED - a GET of
// /auth/v1/.well-known/jwks.json on a cold isolate, parsed as JSON. A JWT has to
// be parsed as JSON (the format is JSON); the key set does not, and a function
// that fetches the list of keys it trusts trusts whatever answers that fetch.
// So the keys are configuration now, in a form that is not JSON:
//
//   JWT_VERIFY_KEYS = <kid>:<alg>:<base64 SPKI>[,<kid>:<alg>:<base64 SPKI>...]
//
//     kid   the key id a token's header names
//     alg   ES256 or RS256
//     SPKI  the public key as DER SubjectPublicKeyInfo, standard base64 - the
//           body of a PEM "PUBLIC KEY" block, which is what crypto.subtle
//           imports as 'spki' for both algorithms
//
//   JWT_VERIFY_KEYS = gotrue
//
//     No key is pinned and EVERY token is verified at GoTrue (the fallback
//     below). For a stack that signs with the legacy HS256 secret, where there
//     is no public key to pin: a local `supabase start`.
//
// More than one key is how a rotation works: the standby key is pinned beside
// the current one BEFORE GoTrue starts signing with it, and the old one is
// dropped once its last token has expired. A token naming a kid that is not
// pinned is not accepted locally; it goes to GoTrue, with a warning that names
// the kid, so a rotation done out of order is slow and noisy rather than an
// outage.
//
// The public half only: nothing here can sign a token.
//
// SETTING IT (the hosted project, once, BEFORE the function that reads it is
// deployed - without it every signed-in request is refused):
//
//   curl -s "https://<project-ref>.supabase.co/auth/v1/.well-known/jwks.json" | node -e '
//     const { createPublicKey } = require("crypto"); let s = "";
//     process.stdin.on("data", (d) => s += d).on("end", () => console.log(JSON.parse(s).keys.map((k) =>
//       `${k.kid}:${k.kty === "EC" ? "ES256" : "RS256"}:${createPublicKey({ key: k, format: "jwk" })
//         .export({ type: "spki", format: "der" }).toString("base64")}`).join(",")))'
//   supabase secrets set JWT_VERIFY_KEYS='<that line>' --project-ref <project-ref>
//
// (That one-liner reads the JWKS, which is JSON, on the operator's machine, once
// per rotation. Nothing shipped does.) An empty key list from the first command
// means the project still signs with the legacy HS256 secret: either turn on
// asymmetric signing keys first, or set JWT_VERIFY_KEYS=gotrue.
//
// ROTATING THE SIGNING KEY, in this order:
//   1. Create the standby key in the dashboard (Authentication > JWT signing keys).
//      The JWKS now lists it beside the current one.
//   2. Run the two commands above again: both keys are pinned.
//   3. Rotate in the dashboard. Tokens signed by the new key verify at once.
//   4. When the old key is revoked (its last token has expired), run the two
//      commands a third time to drop it.
// Skipping step 2 is not an outage: tokens under the new kid go to GoTrue until
// the key is pinned, and the log says which kid to pin.
//
// A LOCAL STACK (`supabase start` + `functions serve`) signs with HS256 unless
// told otherwise: put JWT_VERIFY_KEYS=gotrue in the env file the functions are
// served with.
// ============================================================================

/** JWT_VERIFY_KEYS is unset, or is not a key list: no token can be judged. */
export class AuthNotConfigured extends Error {
    constructor(why: string) {
        super(`auth is not configured: JWT_VERIFY_KEYS ${why}`);
        this.name = 'AuthNotConfigured';
    }
}

interface PinnedKey { alg: Alg; spki: Uint8Array<ArrayBuffer> }
/** null: the configuration says `gotrue`, so nothing is verified locally. */
type Pinned = Map<string, PinnedKey> | null;

const GOTRUE_ONLY = 'gotrue';
let pinnedFrom: string | undefined;        // the configuration text `pinned` was read from
let pinned: Pinned = null;

/** The key list a configuration value states. Throws AuthNotConfigured for one that is not a key list. */
export function parseVerifyKeys(text: string | undefined): Pinned {
    const value = (text ?? '').trim();
    if (!value) throw new AuthNotConfigured('is not set');
    if (value === GOTRUE_ONLY) return null;
    const keys = new Map<string, PinnedKey>();
    for (const entry of value.split(',')) {
        const parts = entry.trim().split(':');
        if (parts.length !== 3) throw new AuthNotConfigured(`has an entry that is not <kid>:<alg>:<base64 SPKI> (entry ${keys.size + 1})`);
        const [kid, alg, b64] = parts;
        if (!kid) throw new AuthNotConfigured(`has an entry with no kid (entry ${keys.size + 1})`);
        if (!isAlg(alg)) throw new AuthNotConfigured(`names an algorithm that is not ES256 or RS256 for kid ${kid}`);
        if (keys.has(kid)) throw new AuthNotConfigured(`pins kid ${kid} twice`);
        let spki: Uint8Array<ArrayBuffer>;
        try {
            const bin = atob(b64);
            spki = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) spki[i] = bin.charCodeAt(i);
        } catch {
            throw new AuthNotConfigured(`holds a key that is not base64 for kid ${kid}`);
        }
        if (spki.length === 0) throw new AuthNotConfigured(`holds an empty key for kid ${kid}`);
        keys.set(kid, { alg, spki });
    }
    return keys;
}

/** The pinned keys of this isolate's configuration, read once per value. Throws AuthNotConfigured, and says so in the log. */
function pinnedKeys(): Pinned {
    const text = Deno.env.get('JWT_VERIFY_KEYS');
    if (pinnedFrom !== undefined && text === pinnedFrom) return pinned;
    try {
        pinned = parseVerifyKeys(text);
    } catch (e) {
        console.error(`[auth] ${(e as Error).message}. Every request is refused until it is set `
            + '(see _shared/adapter/auth.ts: <kid>:<alg>:<base64 SPKI>, comma separated, or `gotrue`).');
        throw e;
    }
    pinnedFrom = text;
    importedByKidAlg.clear();
    if (pinned === null) console.warn('[auth] JWT_VERIFY_KEYS is `gotrue`: no key is pinned, every token is verified at GoTrue (slow)');
    return pinned;
}

// A test's key set: Web Crypto JWKs (what importKey takes) by the key id they
// are served under. Production reads no JWK: its keys are the pinned SPKIs.
type Jwk = JsonWebKey & { kid?: string };
interface Jwks { keys: Jwk[]; }

// CryptoKeys imported under a specific alg, cached across invocations on a warm
// isolate. A pinned key states its algorithm, and a token must name the same
// one: an EC key can never satisfy an RS256 header, or the reverse. (A test's
// JWK carries no `alg` - per RFC 7517 it is optional - so there the algorithm
// comes from the already validated and pinned token header, and a wrong key
// type for it makes importKey throw, which rejects the token.)
const importedByKidAlg = new Map<string, CryptoKey>(); // `${kid}:${alg}` → key
let injectedJwks: Jwks | null = null; // test hook (see __setJwksForTest); bypasses the configuration

const b64urlToBytes = (s: string): Uint8Array<ArrayBuffer> => {
    const b64 = s.replace(/-/g, '+').replace(/_/g, '/');
    const bin = atob(b64 + '='.repeat((4 - (b64.length % 4)) % 4)); // atob throws on malformed → caller catches
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
};
const b64urlToString = (s: string): string => new TextDecoder().decode(b64urlToBytes(s));

// Kids already warned about, so a rotation in progress logs once per isolate
// and not once per request.
const warnedKids = new Set<string>();

// Resolve (and cache) the verify key for a header `kid` under `alg` (from the
// token header). Returns null if the kid is not pinned, is pinned for another
// algorithm, or the key cannot be imported under that alg (wrong key type →
// algorithm-substitution rejected). Never throws: an unreadable configuration
// reads as no key here, and getAuthenticatedUser has already refused it loudly.
async function keyForKid(kid: string, alg: Alg): Promise<CryptoKey | null> {
    const cacheKey = `${kid}:${alg}`;
    try {
        // The configuration is read BEFORE the cache: a key imported under one
        // configuration is not a key under another (pinnedKeys clears the cache
        // when the value changes).
        const keys = injectedJwks ? null : pinnedKeys();
        const cached = importedByKidAlg.get(cacheKey);
        if (cached) return cached;

        let key: CryptoKey;
        if (injectedJwks) {
            const jwk = injectedJwks.keys.find((k) => k.kid === kid);
            if (!jwk) return null;
            key = await crypto.subtle.importKey('jwk', jwk, IMPORT_PARAMS[alg], false, ['verify']);
        } else {
            if (keys === null) return null;                 // `gotrue`: nothing is verified locally
            const entry = keys.get(kid);
            if (!entry) {
                if (!warnedKids.has(kid)) {
                    warnedKids.add(kid);
                    console.warn(`[auth] a token names kid ${kid}, which JWT_VERIFY_KEYS does not pin: `
                        + 'verified at GoTrue instead. If the signing key was rotated, pin the new one.');
                }
                return null;
            }
            if (entry.alg !== alg) return null;             // the token claims another algorithm for this key
            key = await crypto.subtle.importKey('spki', entry.spki, IMPORT_PARAMS[alg], false, ['verify']);
        }
        importedByKidAlg.set(cacheKey, key);
        return key;
    } catch {
        return null; // not a valid key for this alg, or no readable configuration
    }
}

interface VerifiedClaims {
    sub: string;
    aud?: string | string[];
    role?: string;
    email?: string;
    phone?: string;
    exp?: number;
    nbf?: number;
    app_metadata?: Record<string, unknown>;
    user_metadata?: Record<string, unknown>;
    [claim: string]: unknown;
}

// Decode — WITHOUT verifying — the `sub` claim from a compact JWS. This exists
// purely for latency: it lets a handler start subject-scoped work (e.g. a DB
// read for the caller's own rows) in parallel with signature verification. It is
// NOT authorization — the signature is unchecked, so a caller could claim any
// sub. Every caller MUST await verifyJwtLocal / getAuthenticatedUser and refuse
// to return anything if that fails. Never throws.
export function unverifiedSubFromToken(token: string): string | null {
    const parts = token.split('.');
    if (parts.length !== 3) return null;
    try {
        const payload = JSON.parse(b64urlToString(parts[1])) as { sub?: unknown };
        return typeof payload?.sub === 'string' && payload.sub ? payload.sub : null;
    } catch {
        return null;
    }
}

// Verify a compact JWS locally and return its claims, or null if the token is
// malformed, uses a disallowed algorithm, is signed by an unknown key, fails the
// signature check, or is expired / not-yet-valid. Never throws.
export async function verifyJwtLocal(token: string): Promise<VerifiedClaims | null> {
    const parts = token.split('.');
    if (parts.length !== 3) return null;
    const [h, p, s] = parts;

    let header: { alg?: unknown; kid?: unknown };
    try {
        header = JSON.parse(b64urlToString(h));
    } catch {
        return null;
    }
    // Reject anything but our asymmetric algs BEFORE any crypto (blocks HS256/none).
    if (!isAlg(header.alg) || typeof header.kid !== 'string') return null;
    const alg = header.alg;

    const key = await keyForKid(header.kid, alg);
    if (!key) return null;

    let sig: Uint8Array<ArrayBuffer>;
    try {
        sig = b64urlToBytes(s);
    } catch {
        return null;
    }
    const data = new TextEncoder().encode(`${h}.${p}`);
    let ok = false;
    try {
        ok = await crypto.subtle.verify(VERIFY_PARAMS[alg], key, sig, data);
    } catch {
        return null;
    }
    if (!ok) return null;

    let claims: VerifiedClaims;
    try {
        claims = JSON.parse(b64urlToString(p));
    } catch {
        return null;
    }
    if (!claims || typeof claims.sub !== 'string' || !claims.sub) return null;

    const nowSec = Math.floor(Date.now() / 1000);
    const SKEW = 30; // seconds of tolerated clock skew
    if (typeof claims.exp === 'number' && nowSec > claims.exp + SKEW) return null;
    if (typeof claims.nbf === 'number' && nowSec + SKEW < claims.nbf) return null;

    return claims;
}

// Test hook: inject a key set so verifyJwtLocal runs without any configuration.
// Passing null restores the configured path (JWT_VERIFY_KEYS) and clears cached keys.
export function __setJwksForTest(jwks: Jwks | null): void {
    injectedJwks = jwks;
    importedByKidAlg.clear();
    pinnedFrom = undefined;
    warnedKids.clear();
}

// Reconstruct the minimal User the rest of the server reads (id +
// user_metadata.username) from verified JWT claims. The token already carries
// these — no need to round-trip to GoTrue to fetch them again.
function claimsToUser(claims: VerifiedClaims): User {
    const user: User = {
        id: claims.sub,
        aud: (claims.aud as string) ?? '',
        role: claims.role ?? 'authenticated',
        email: claims.email ?? '',
        phone: claims.phone ?? '',
        app_metadata: claims.app_metadata ?? {},
        user_metadata: claims.user_metadata ?? {},
        created_at: '',
    };
    return user;
}
