import { wrap400, personalize_game, verify_game_id, verify_player_in_game } from '../_shared/utils.ts';

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { createClient, User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

serve(wrap400(async (req) => {
    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    const user: User = await getAuthenticatedUser(req);
    const user_id = user.id;
    const { game_id } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id); 
    // ENSURE there is a policy like only game people can see the game, then remove this
    await verify_player_in_game(game_id, user_id);

    const { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    const response = new Response(JSON.stringify({
        game: personalize_game(game, user_id)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    return response;
}));
