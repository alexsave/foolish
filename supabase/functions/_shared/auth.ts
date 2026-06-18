import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

/**
 * Authenticates a user from the request's Authorization header.
 *
 * Fast path — `auth.getClaims(token)`: with asymmetric JWT signing keys
 * (ES256/RS256) enabled on the project, this verifies the token LOCALLY against
 * the project's public JWKS (cached after first fetch, keyed by the token's
 * `kid`, rotation-aware). No GoTrue round-trip — `getUser` was costing ~850ms
 * on every authenticated call, the dominant slice of "create game is slow".
 * Verification uses the PUBLIC key only, so this can't be used to forge tokens;
 * it just checks the signature Supabase already made. (See
 * https://supabase.com/blog/jwt-signing-keys.)
 *
 * Fallback — `auth.getUser(token)`: if the project still uses the legacy HS256
 * shared secret (no JWKS to verify against locally), or getClaims can't verify
 * for any reason, fall back to the original server-side verification so auth
 * never silently breaks. Slower, but correct.
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
    try {
        const getClaims = (supabaseClient.auth as any).getClaims;
        if (typeof getClaims === 'function') {
            const { data, error } = await getClaims.call(supabaseClient.auth, token);
            if (!error && data?.claims?.sub) {
                return claimsToUser(data.claims);
            }
            // No throw: a verification miss (e.g. legacy HS256 project, or no
            // cached key for this kid) falls through to the authoritative path.
        }
    } catch (e) {
        console.warn('getClaims fast-path unavailable, falling back to getUser:', (e as Error).message);
    }

    // Authoritative fallback: server-side verification at GoTrue.
    const { data: { user }, error } = await supabaseClient.auth.getUser(token);

    if (error) {
        console.error('Authentication error:', error.message);
        throw new Error(`Invalid token: ${error.message}`);
    }

    if (!user) {
        throw new Error('User not found');
    }

    return user;
}

// Reconstruct the minimal User the rest of the server reads (id +
// user_metadata.username) from verified JWT claims. The token already carries
// these — no need to round-trip to GoTrue to fetch them again.
function claimsToUser(claims: any): User {
    return {
        id: claims.sub,
        aud: claims.aud ?? '',
        role: claims.role ?? 'authenticated',
        email: claims.email ?? '',
        phone: claims.phone ?? '',
        app_metadata: claims.app_metadata ?? {},
        user_metadata: claims.user_metadata ?? {},
        created_at: '',
    } as User;
}
