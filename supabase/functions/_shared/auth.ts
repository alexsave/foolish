import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import type { User } from 'jsr:@supabase/supabase-js'; // type-only → erased at compile time, no runtime/boot cost

/**
 * Authenticates a user from the request's Authorization header.
 *
 * Fast path — LOCAL asymmetric verification (verifyJwtLocal below). With
 * asymmetric JWT signing keys (ES256/RS256) enabled on the project, the token is
 * verified against the project's public JWKS right here: no GoTrue round-trip
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
 * legacy HS256 shared secret (no public JWKS to verify against locally), or
 * local verification fails for any reason, fall back to server-side
 * verification so auth never silently breaks. supabase-js is imported LAZILY
 * here, so the common (asymmetric) case never loads it.
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
        throw new Error('No authorization header');
    }

    const token = authHeader.replace('Bearer ', '');

    // Fast path: local signature verification via JWKS (asymmetric keys).
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
        throw new Error(`Invalid token: ${(e as Error).message}`);
    }
}

// ============================================================================
// Local JWT verification. The cryptography is the platform's Web Crypto; this
// only parses the JWS envelope and pins the algorithm. No supabase-js.
// ============================================================================

// The ONLY algorithms accepted — both ASYMMETRIC, verified with a PUBLIC key.
// This is the alg-confusion defense: HS256/HS384/HS512 (symmetric) and "none"
// are absent, so we never treat the public JWKS key as an HMAC secret, and we
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

interface Jwk { kid?: string; kty?: string; [k: string]: unknown; }
interface Jwks { keys: Jwk[]; }

// Raw JWKs by kid, and CryptoKeys imported under a specific alg, cached across
// invocations on a warm isolate. We DON'T key off the JWK's own `alg` field:
// per RFC 7517 it is OPTIONAL, and Supabase's JWKS omits it — so the algorithm
// comes from the (already validated + pinned) token header, and the JWK is
// imported under that alg. A wrong key type for that alg makes importKey throw,
// which rejects the token, so this is also the algorithm-substitution bind
// (an EC key can't satisfy an RS256 header, or vice versa).
const jwkByKid = new Map<string, Jwk>();
const importedByKidAlg = new Map<string, CryptoKey>(); // `${kid}:${alg}` → key
let lastJwksFetch = 0;
let injectedJwks: Jwks | null = null; // test hook (see __setJwksForTest); bypasses the network

const b64urlToBytes = (s: string): Uint8Array => {
    const b64 = s.replace(/-/g, '+').replace(/_/g, '/');
    const bin = atob(b64 + '='.repeat((4 - (b64.length % 4)) % 4)); // atob throws on malformed → caller catches
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
};
const b64urlToString = (s: string): string => new TextDecoder().decode(b64urlToBytes(s));

async function fetchJwks(): Promise<Jwks | null> {
    const url = Deno.env.get('SUPABASE_URL');
    if (!url) return null;
    const apikey = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || Deno.env.get('SUPABASE_ANON_KEY') || '';
    try {
        const res = await fetch(`${url}/auth/v1/.well-known/jwks.json`, { headers: apikey ? { apikey } : {} });
        if (!res.ok) {
            console.warn(`[auth] JWKS fetch HTTP ${res.status}`);
            return null;
        }
        return await res.json() as Jwks;
    } catch (e) {
        console.warn('[auth] JWKS fetch failed:', (e as Error).message);
        return null;
    }
}

function cacheJwks(jwks: Jwks): void {
    for (const jwk of jwks.keys ?? []) {
        if (typeof jwk.kid === 'string') jwkByKid.set(jwk.kid, jwk);
    }
}

// The raw JWK for a header `kid`. On a miss, refetch the JWKS at most once per
// minute so a bogus/rotated kid can't force a JWKS fetch on every request.
async function jwkForKid(kid: string): Promise<Jwk | null> {
    const cached = jwkByKid.get(kid);
    if (cached) return cached;

    if (injectedJwks) {
        cacheJwks(injectedJwks);
        return jwkByKid.get(kid) ?? null;
    }

    const now = Date.now();
    if (jwkByKid.size > 0 && now - lastJwksFetch < 60_000) return null; // throttle refetch for unknown kids
    lastJwksFetch = now;
    const jwks = await fetchJwks();
    if (jwks) cacheJwks(jwks);
    return jwkByKid.get(kid) ?? null;
}

// Resolve (and cache) the verify key for a header `kid`, imported under `alg`
// (from the token header). Returns null if the kid is unknown or the JWK can't
// be imported under that alg (wrong key type → algorithm-substitution rejected).
async function keyForKid(kid: string, alg: Alg): Promise<CryptoKey | null> {
    const cacheKey = `${kid}:${alg}`;
    const cached = importedByKidAlg.get(cacheKey);
    if (cached) return cached;

    const jwk = await jwkForKid(kid);
    if (!jwk) return null;
    try {
        const key = await crypto.subtle.importKey('jwk', jwk as JsonWebKey, IMPORT_PARAMS[alg], false, ['verify']);
        importedByKidAlg.set(cacheKey, key);
        return key;
    } catch {
        return null; // JWK is not a valid key for this alg
    }
}

export interface VerifiedClaims {
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

    let sig: Uint8Array;
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

// Test hook: inject a JWKS so verifyJwtLocal runs entirely offline (no fetch,
// no Deno). Passing null restores the network path and clears cached keys.
export function __setJwksForTest(jwks: Jwks | null): void {
    injectedJwks = jwks;
    jwkByKid.clear();
    importedByKidAlg.clear();
    lastJwksFetch = 0;
}

// Reconstruct the minimal User the rest of the server reads (id +
// user_metadata.username) from verified JWT claims. The token already carries
// these — no need to round-trip to GoTrue to fetch them again.
function claimsToUser(claims: VerifiedClaims): User {
    return {
        id: claims.sub,
        aud: (claims.aud as string) ?? '',
        role: claims.role ?? 'authenticated',
        email: claims.email ?? '',
        phone: claims.phone ?? '',
        app_metadata: claims.app_metadata ?? {},
        user_metadata: claims.user_metadata ?? {},
        created_at: '',
    } as User;
}
