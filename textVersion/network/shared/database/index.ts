// Database layer and related functions
import WebSocket from 'ws';
import { Game, User, GameMessage, PrivateMessage, LobbyGame, GAME_STATUS, ChatMessage } from '../types';

// Basically the supabase schemas
export const database = {
    games: {} as Record<string, Game>,
    users: {} as Record<string, User>,
    player_games: {} as Record<string, string[]>,
    public_game_channel: [] as GameMessage[],
    private_user_channel: [] as PrivateMessage[],
    name_to_id: {} as Record<string, string>,
    user_ports: {} as Record<string, WebSocket>,
    // also a "channel"
    chat_messages: [] as ChatMessage[]
}

// Database getter methods - these will be replaced with Supabase calls later
export const getGames = () => database.games;
export const getUsers = () => database.users;
export const getPlayerGames = () => database.player_games;
export const getPublicGameChannel = () => database.public_game_channel;
export const getPrivateUserChannel = () => database.private_user_channel;
export const getNameToId = () => database.name_to_id;
export const getUserPorts = () => database.user_ports;
export const getChatMessages = () => database.chat_messages;

// throw error if not found
export const verify_game_id = (game_id: string) => {
    const games = getGames();
    if (!games[game_id]) {
        throw new Error(`Game ${game_id} not found`);
    }
    return game_id;
}

export const verify_player_in_game = (game_id: string, player_id: string) => {
    const player_games = getPlayerGames();
    if (!player_games[player_id] || !player_games[player_id].includes(game_id)) {
        throw new Error(`Player ${player_id} is not in game ${game_id}`);
    }
}

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
        players: game.players.map(player => ({ name: player.name, status: player.status, id: player.id })),
        status: game.status === GAME_STATUS.WAITING ? GAME_STATUS.WAITING : GAME_STATUS.PLAYING
    };
} 