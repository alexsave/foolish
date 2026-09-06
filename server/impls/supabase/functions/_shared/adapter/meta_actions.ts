// Lobby / "game meta" actions, consolidated. These used to be separate edge
// functions (start, add-bot, exit, continue, join, rearrange-hand,
// rearrange-players, update-name); folding them into one `meta` endpoint
// (dispatched on body.type) cuts the function count for faster deploys — the same
// move the `action` endpoint made for gameplay. The per-action logic is unchanged;
// only the packaging is consolidated. Each handler mutates params.game and returns
// {game, events}; executeWithGameLock (via wrap400) does the commit.

import { ExecutionParams, broadcastToGameUser, PackedPayloadExtra } from './utils.ts';
import { ANIMATION_EVENT_TYPE, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, SERVER_EVENT_TYPE, AnimationEvent, Game } from '@api/core/types.ts';
import { cloneGame, verify_player_in_game } from '@api/common/common_utils.ts';
import { packedProducts, start_game_packed } from '@api/common/game_lifecycle.ts';
import { MAX_PLAYERS } from '@api/core/constants.ts';
import { handleRearrangeHand as applyRearrangeHand } from '@api/common/actions/rearrange.ts';
import { runPackedRearrange, PackedRunOk, kernelResetToLobby } from '@sdk/ts/wasm/engine.ts';
import { bytesToHex } from '@api/common/replay/codec.ts';
import { bytesToBareHex } from '@sdk/ts/wire/bytes.ts';
import { logsFromKernelExport } from '@sdk/ts/wire/logwire.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

// Kernel run -> the commit/broadcast products executeWithGameLock consumes.
// The roster, for a broadcast that changes it. Recipients decode the deal
// against this rather than against whatever roster they happen to hold.
const rosterExtra = (game: Game): PackedPayloadExtra => ({
    r: {
        name: game.name,
        players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    },
});

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// `deleted` marks that the handler removed the games row itself (last player
// exiting); executeWithGameLock then skips the version-CAS commit, which would
// otherwise miss the deleted row, read as a conflict, and 400 a clean teardown.
type Result = { game: Game; events: AnimationEvent[]; deleted?: boolean; packed?: PackedOpProducts };

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
        // Kernel-packed deal: every recipient already knows this roster (the
        // join/add-bot broadcasts carried it), so the fattest broadcast in
        // the game — per-viewer DEAL/FLIPPED streams — goes out as kernel
        // bytes with no JS AnimationEvents in between.
        return { game, events: [], packed: packedProducts(start_game_packed(game)) };
    }

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${player?.name} is ready`,
        game_state: cloneGame(game)
    }] };
}

// ---- add bot ---------------------------------------------------------------
export async function handleAddBot({ body, game, user, botsPrefetch }: ExecutionParams): Promise<Result> {
    const { game_id, bot_id } = body;

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    // Cap the lobby at the engine's player limit. Without this a client can
    // flood add-bot (the roster has dozens of bots) into an oversized lobby;
    // starting it then deals more hands than the deck holds and crashes.
    if (game.players.length >= MAX_PLAYERS) {
        throw new Error(`Game is full (max ${MAX_PLAYERS} players)`);
    }

    // Prefer the roster read wrap400 kicked off in parallel with the game load
    // (one fewer serial round-trip); fall back to an inline fetch for direct
    // callers / tests that invoke this handler without the prefetch.
    const { data: allBots, error } = await (botsPrefetch ?? supabaseClient.from('bots').select('*'));
    if (error || !allBots) {
        throw new Error(`Failed to fetch bots`);
    }

    const existingBotIds = game.players.filter(p => p.is_ai).map(p => p.player_id);
    const availableBots = allBots.filter(bot => !existingBotIds.includes(bot.id));

    if (availableBots.length === 0) {
        throw new Error(`No available bots to add to the game`);
    }

    // If the caller named a specific bot (lobby bot picker), add exactly that one;
    // it must pass the same availability gate (not already in game).
    // Without a bot_id we keep the original random pick (backwards compatible).
    const availableBot = bot_id
        ? availableBots.find(b => b.id === bot_id)
        : availableBots[Math.floor(Math.random() * availableBots.length)];

    if (!availableBot) {
        throw new Error(`Bot ${bot_id} is not available to add to this game`);
    }

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
        // Kernel-packed deal, like handleStart's branch. This one bundles a
        // ROSTER change (the bot just joined), so the broadcast carries it -
        // which is the only thing that used to keep this start on the JS
        // AnimationEvent path and its TypeScript re-encoder.
        return { game, events: [], packed: packedProducts(start_game_packed(game), rosterExtra(game)) };
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
        // No explicit bot_hands DELETE here: the commit that follows is a lobby
        // commit (status=waiting), and commit_game now prunes bot_hands not in the
        // post-removal roster in the same transaction — one fewer round-trip.
    } else {
        if (player_id === undefined) player_id = user_id;
        verify_player_in_game(game, player_id);
        const userPlayer = game.players.find(player => player.player_id === player_id);
        if (userPlayer) exitedPlayerName = userPlayer.name;
        game.players = game.players.filter(player => player.player_id !== player_id);
        await supabaseClient.from('player_hands').delete().eq('game_id', game.id).eq('player_id', player_id);
    }

    if (game.players.length === 0) {
        // The membership rows cascade with the game (ON DELETE CASCADE), and
        // game_decks is gone (migration 20260906120000), so this is one delete.
        await supabaseClient.from('games').delete().eq('id', game.id);
        return { game, events: [], deleted: true };
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

    // Determine the winner / fool BEFORE the reset below clobbers the
    // finished-game fields. The winner is the FIRST player to shed their
    // cards — elimination_order[0] — not the first OUT seat in table order
    // (those differ in 3+ player games). The fool is whoever is still IN
    // (kernel finalize parks seats, so fall back to "not in
    // elimination_order" when statuses were already reset).
    const winner = game.players.find(p => p.player_id === game.elimination_order[0])
        ?? game.players.find(p => p.status === PLAYER_STATUS.OUT);
    const fool = game.players.find(p => p.status === PLAYER_STATUS.IN)
        ?? game.players.find(p => !game.elimination_order.includes(p.player_id));
    let message = `Game ${game.id} has been reset for another round`;
    if (winner) message = `Player ${winner.name} won! Game reset for another round`;
    else if (fool) message = `Player ${fool.name} was the fool! Game reset for another round`;

    // The reset itself is the kernel's (c/src/game.c game_reset_to_lobby): which
    // seats come back READY, which volatile round fields are cleared, and that
    // the good-players set is cleared HERE rather than left for the next deal.
    // That last one is a fix, not a port - this handler used to leave
    // good_players/good_timestamp set, so the lobby it broadcast still showed
    // the finished round's goods until the next deal cleared them. The browser's
    // optimistic mirror always cleared them, which is why the snap was invisible
    // until you looked.
    kernelResetToLobby(game);

    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message,
        game_state: game
    }] };
}

// ---- join ------------------------------------------------------------------
function handleJoin({ user, user_name, body, game }: ExecutionParams): Result {
    const user_id = user.id;
    const { game_id } = body;

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }
    if (game.players.some(p => p.player_id === user_id)) {
        throw new Error(`Player ${user_id} is already in game ${game_id}`);
    }
    if (game.players.length >= MAX_PLAYERS) {
        throw new Error(`Game is full (max ${MAX_PLAYERS} players)`);
    }

    game.players.push({
        player_id: user_id,
        name: user_name,
        status: PLAYER_STATUS.IDLE,
        is_ai: false,
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        strategy_key: STRATEGY_KEY.HUMAN
    });

    // saveCompleteGame (via executeWithGameLock) persists the new players array
    // and upserts the joiner's player_hands row — no extra DB ops needed here.
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${user_name} joined the game`,
        game_state: game
    }] };
}

// ---- rearrange hand --------------------------------------------------------
function handleRearrangeHand({ user, user_name, game, body }: ExecutionParams): Result {
    // Reorder the caller's hand. For a dealt game the permutation validation
    // — the load-bearing uniqueness check that prevents minting duplicate
    // cards via repeated indices — runs INSIDE the kernel
    // (wasm_rearrange_hand), and the reordered durable blob comes straight
    // back; only the payload shape is checked in TS. Lobby/legacy games keep
    // the JS path (their commit writes the hand tables, not the blob).
    let packed: PackedOpProducts | undefined;
    if (game.status === GAME_STATUS.PLAYING) {
        const indices = body.card_indices;
        if (!Array.isArray(indices) ||
            !indices.every((i: unknown) => Number.isInteger(i) && (i as number) >= 0 && (i as number) <= 0xff)) {
            throw new Error('Invalid card indices');
        }
        const seat = game.players.findIndex(p => p.player_id === user.id);
        if (seat < 0) throw new Error('You are not in this game');
        const result = runPackedRearrange(game, seat, indices as number[]);
        if (!result) throw new Error('Invalid card indices');
        // Keep the in-memory game in step for the commit's public dual.
        game.players[seat].hand = result.post.players[seat].hand;
        packed = {
            ended: false,
            stateHex: bytesToHex(result.stateBlob),
            logsHex: null,
            nEvents: 0,
            events: new Map(),
        };
    } else {
        applyRearrangeHand(game, user.id, body.card_indices);
    }

    // Targeted broadcast only to the caller (their hand order is private); the
    // committed game itself is broadcast by executeWithGameLock.
    broadcastToGameUser(game, SERVER_EVENT_TYPE.HAND_REARRANGED, {
        message: `${user_name} rearranged their hand`
    }, user.id);

    return { game, events: [], packed };
}

// ---- rearrange players (lobby seating order) -------------------------------
function handleRearrangePlayers({ user, body, game }: ExecutionParams): Result {
    const user_id = user.id;
    const { new_order } = body;

    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only rearrange players during game lobby`);
    }
    if (!Array.isArray(new_order) || new_order.length !== game.players.length) {
        throw new Error(`New order must contain exactly ${game.players.length} player IDs`);
    }
    for (const player_id of new_order) {
        if (!game.players.some(p => p.player_id === player_id)) {
            throw new Error(`Player ID ${player_id} not found in game`);
        }
    }

    game.players = new_order.map(player_id => game.players.find(p => p.player_id === player_id)!);

    const userName = game.players.find(p => p.player_id === user_id)?.name || 'Someone';
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${userName} rearranged the player order`,
        game_state: game
    }] };
}

// ---- update game name ------------------------------------------------------
function handleUpdateName({ user, body, game }: ExecutionParams): Result {
    const user_id = user.id;
    const { new_name } = body;

    verify_player_in_game(game, user_id);

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Can only update name during game lobby`);
    }
    if (!new_name || typeof new_name !== 'string' || new_name.trim().length === 0) {
        throw new Error('New name must be a non-empty string');
    }
    if (new_name.length > 50) {
        throw new Error('Name must be 50 characters or less');
    }

    const oldName = game.name;
    game.name = new_name.trim();

    const userName = game.players.find(p => p.player_id === user_id)?.name || 'Someone';
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${userName} changed game name from "${oldName}" to "${game.name}"`,
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
        case 'join': return handleJoin(params);
        case 'rearrange-hand': return handleRearrangeHand(params);
        case 'rearrange-players': return handleRearrangePlayers(params);
        case 'update-name': return handleUpdateName(params);
        default: throw new Error(`unknown meta action type: ${params.body?.type}`);
    }
}
