// Lobby / "game meta" actions, consolidated. These used to be four separate edge
// functions (start, add-bot, exit, continue); folding them into one `meta`
// endpoint (dispatched on body.type) cuts the function count for faster deploys —
// the same move the `action` endpoint made for gameplay. The per-action logic is
// unchanged; only the packaging is consolidated. Each handler mutates params.game
// and returns {game, events}; executeWithGameLock (via wrap400) does the commit.

import { ExecutionParams } from './utils.ts';
import { ANIMATION_EVENT_TYPE, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, AnimationEvent, Game } from './types.ts';
import { start_game, cloneGame, verify_player_in_game } from './common_utils.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Only this user can add GPT bots (to control API costs)
const GPT_ALLOWED_USER_ID = '60a5c562-0922-40a6-b416-77e3285d87b2';

type Result = { game: Game; events: AnimationEvent[] };

// ---- start / ready ---------------------------------------------------------
function handleStart({ user, game }: ExecutionParams): Result {
    const user_id = user.id;
    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.WAITING) {
        return { game, events: [] };
    }

    const player = game.players.find(p => p.player_id === user_id);
    if (player) {
        player.status = PLAYER_STATUS.READY;
    }

    const allPlayersReady = game.players.every(p => p.status === PLAYER_STATUS.READY) && game.players.length >= 2;
    if (allPlayersReady) {
        return { game, events: start_game(game) };
    }

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${player?.name} is ready`,
        game_state: cloneGame(game)
    }] };
}

// ---- add bot ---------------------------------------------------------------
export async function handleAddBot({ body, game, user }: ExecutionParams): Promise<Result> {
    const { game_id } = body;

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    const { data: allBots, error } = await supabaseClient.from('bots').select('*');
    if (error || !allBots) {
        throw new Error(`Failed to fetch bots`);
    }

    const existingBotIds = game.players.filter(p => p.is_ai).map(p => p.player_id);
    const availableBots = allBots.filter(bot => {
        if (existingBotIds.includes(bot.id)) return false;
        if (bot.strategy_key === STRATEGY_KEY.GPT && user.id !== GPT_ALLOWED_USER_ID) return false;
        return true;
    });

    if (availableBots.length === 0) {
        throw new Error(`No available bots to add to the game`);
    }

    const availableBot = availableBots[Math.floor(Math.random() * availableBots.length)];

    game.players.push({
        player_id: availableBot.id,
        name: availableBot.nickname,
        status: PLAYER_STATUS.READY,
        is_ai: true,
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        strategy_key: availableBot.strategy_key
    });

    const allPlayersReady = game.players.every(p => p.status === PLAYER_STATUS.READY) && game.players.length >= 2;
    if (allPlayersReady) {
        return { game, events: start_game(game) };
    }

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `Bot ${availableBot.nickname} joined the game`,
        game_state: cloneGame(game)
    }] };
}

// ---- exit (leave / remove bot) ---------------------------------------------
export async function handleExit({ user, body, game }: ExecutionParams): Promise<Result> {
    const user_id = user.id;
    let { bot_id, player_id } = body;

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game.id} is not in the lobby`);
    }

    let exitedPlayerName = '';

    if (bot_id) {
        const botPlayer = game.players.find(player => player.player_id === bot_id && player.is_ai);
        if (!botPlayer) {
            throw new Error(`Bot ${bot_id} is not in the game`);
        }
        exitedPlayerName = botPlayer.name;
        game.players = game.players.filter(player => player.player_id !== bot_id);
        await supabaseClient.from('bot_hands').delete().eq('game_id', game.id).eq('bot_id', bot_id);
    } else {
        if (player_id === undefined) player_id = user_id;
        verify_player_in_game(game, player_id);
        const userPlayer = game.players.find(player => player.player_id === player_id);
        if (userPlayer) exitedPlayerName = userPlayer.name;
        game.players = game.players.filter(player => player.player_id !== player_id);
        await supabaseClient.from('player_hands').delete().eq('game_id', game.id).eq('player_id', player_id);
    }

    if (game.players.length === 0) {
        await supabaseClient.from('games').delete().eq('id', game.id);
        await supabaseClient.from('game_decks').delete().eq('game_id', game.id);
        return { game, events: [] };
    }

    const playerType = bot_id ? 'Bot' : 'Player';
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${playerType} ${exitedPlayerName} left the game`,
        game_state: game
    }] };
}

// ---- continue (reset after the win screen) ---------------------------------
export function handleContinue({ user, game }: ExecutionParams): Result {
    const user_id = user.id;
    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.GAME_OVER) {
        throw new Error(`Game ${game.id} is not over`);
    }

    game.status = GAME_STATUS.WAITING;
    game.players.forEach(player => {
        player.status = player.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
        player.hand = [];
        player.hand_length = 0;
        player.awaiting_attack = false;
    });

    game.deck = [];
    game.discard_pile_length = 0;
    game.flipped = null;
    game.power_suit = 0;
    game.first_attacker = 0;
    game.defender = 0;
    game.table_battles = [];
    game.elimination_order = [];

    const winner = game.players.find(p => p.status === PLAYER_STATUS.OUT);
    const fool = game.players.find(p => p.status === PLAYER_STATUS.IN);
    let message = `Game ${game.id} has been reset for another round`;
    if (winner) message = `Player ${winner.name} won! Game reset for another round`;
    else if (fool) message = `Player ${fool.name} was the fool! Game reset for another round`;

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message,
        game_state: game
    }] };
}

// Dispatch a meta action by type (shared by the `meta` edge function and tests).
export async function handleMetaAction(params: ExecutionParams): Promise<Result> {
    switch (params.body?.type) {
        case 'start': return handleStart(params);
        case 'add-bot': return await handleAddBot(params);
        case 'exit': return await handleExit(params);
        case 'continue': return handleContinue(params);
        default: throw new Error(`unknown meta action type: ${params.body?.type}`);
    }
}
