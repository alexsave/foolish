import { corsHeaders } from './cors.ts';
import { Game, LobbyGame, GAME_STATUS, Player, OtherPlayer, PLAYER_STATUS, PersonalGame } from './types.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

export const createId = (): string => crypto.randomUUID().slice(0, 6);

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
        id: game.id,
        players: game.players.map(player => ({ name: player.name, status: player.status, id: player.id })),
        status: game.status === GAME_STATUS.WAITING ? GAME_STATUS.WAITING : GAME_STATUS.PLAYING
    };
};

export const wrap400 = (execute: (req: Request) => Promise<Response>) => async (req: Request): Promise<Response> => {
    try {
        return execute(req);
    } catch (e: any) {
        console.error('Error processing request:', {
            name: e.name,
            message: e.message,
            stack: e.stack,
            cause: e.cause
        });

        return new Response(
            JSON.stringify({ error: e.message }),
            { status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' } }
        );
    }
}

export const emailToName = (email: string): string => {
  return email.split('@')[0];
}


export const verify_game_id = async (game_id: string): Promise<void> => {
    const { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        throw new Error(`Game ${game_id} not found`);
    }
}

export const verify_player_in_game = async (game_id: string, player_id: string): Promise<void> => {
    const { data: player_game, error: player_gameError } = await supabaseClient.from('player_games').select('*').eq('game_id', game_id).eq('player_id', player_id).single();
    if (player_gameError) {
        console.error('Error loading player game', player_gameError);
        throw new Error(`Player ${player_id} not in game ${game_id}`);
    }
}

const other_player = (player: Player): OtherPlayer => {
    return { 
        name: player.name, 
        id: player.id, 
        hand_length: player.hand.length, 
        status: player.status === PLAYER_STATUS.AWAITING_ATTACK ? PLAYER_STATUS.IN : player.status 
    };
}

export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    return {
        deck_length: game.deck.length,
        flipped: game.flipped,
        self: game.players.find(player => player.id === player_id)!,
        players: game.players.map(other_player),
        status: game.status,
        first_attacker: game.first_attacker,
        currently_attacked: game.currently_attacked,
        previous_first_attacker: game.previous_first_attacker,
        previous_currently_attacked: game.previous_currently_attacked,
        table_battles: game.table_battles,
        power_suit: game.power_suit
    }
}
