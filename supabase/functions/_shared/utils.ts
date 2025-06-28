import { corsHeaders } from './cors.ts';
import { Game, LobbyGame, GAME_STATUS } from './types.ts';

export const createId = (): string => crypto.randomUUID().slice(0, 6);

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
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