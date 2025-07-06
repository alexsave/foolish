import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

/**
 * Authenticates a user from the request's Authorization header.
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
