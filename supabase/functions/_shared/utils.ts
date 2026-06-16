import { corsHeaders, handleCors } from './cors.ts';
import {
    personalize_game,
    calculateEloChange,
    calculateGameRankings,
    game_done,
    other_player,
} from './common_utils.ts';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PersonalGame, PrivatePlayer, PublicGame, PlayerHand, UserEloRating, BotHand, AnimationEvent, PublicAnimationEvent, PersonalAnimationEvent, ANIMATION_EVENT_TYPE, GameLog, STRATEGY_KEY } from './types.ts';
import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';
import { lockedBotLoop } from './bot_actions.ts';
import { botDiag } from './diag.ts';
import { saveGameLogs, cleanupOldGameLogs, wipeAllGameLogs } from './log_utils.ts';
import { verifyRoundTrip } from './replay/encode.ts';
import { encodeExtras, moveTimesFromLogs } from './replay/extras.ts';
import { base32Decode, bytesToHex } from './replay/codec.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// (game_locks acquire/release removed — concurrency is now optimistic CAS via
// the commit_game RPC; see executeWithGameLock below.)

// Card-conservation audit. The whole game must always hold exactly 36 cards (≤4
// players) or 52 (≥5), with no (suit,value) appearing twice. Returns null when the
// game isn't in active play (a waiting game has no cards dealt). The discard pile
// is stored as a count only (no Card objects), so its cards count toward the total
// but can't be dup-checked — a card duplicated in the deck/hands/table is still
// caught, which is exactly the corruption we're hunting.
const auditCards = (game: Game): {
    ok: boolean; total: number; expected: number;
    enumerated: number; discard: number; duplicates: string[];
} | null => {
    if (game.status !== GAME_STATUS.PLAYING && game.status !== GAME_STATUS.GAME_OVER) {
        return null;
    }
    const cards: Card[] = [];
    cards.push(...game.deck);
    if (game.flipped) cards.push(game.flipped);
    for (const p of game.players) cards.push(...(p.hand ?? []));
    for (const b of game.table_battles) {
        if (b.attack) cards.push(b.attack);
        if (b.defense) cards.push(b.defense);
    }
    const discard = game.discard_pile_length ?? 0;
    const total = cards.length + discard;
    const expected = game.players.length > 4 ? 52 : 36;
    const seen = new Map<string, number>();
    for (const c of cards) {
        const k = `${c.suit}-${c.value}`;
        seen.set(k, (seen.get(k) ?? 0) + 1);
    }
    const duplicates = [...seen.entries()].filter(([, n]) => n > 1).map(([k, n]) => `${k}x${n}`);
    return {
        ok: total === expected && duplicates.length === 0,
        total, expected, enumerated: cards.length, discard, duplicates,
    };
};

// ============================================================================
// CONCURRENCY: optimistic version CAS (replaces the old game_locks table).
//
// We can't hold a real DB lock across the TS load→compute→save: PostgREST gives
// one transaction per call, never spanning the (up to 2s) cordite compute. So
// instead of locking, we load the game WITH its version, compute the move, and
// commit via the commit_game RPC, which only writes if games.version still equals
// what we loaded — then bumps it. That commit is ONE transaction across all
// tables (no torn reads) and is FENCED by the version (a stale/slow execution can
// never overwrite a newer state → the duplicate-card bug is impossible). Nothing
// is held, so nothing can leak or freeze. On conflict we reload and redo.
// (Name kept as executeWithGameLock so callers are unchanged; it no longer locks.)
// ============================================================================
export const executeWithGameLock = async (game_id: string, operation: (game: Game) => Promise<{ game: Game, events: AnimationEvent[] }>, reqId: string = 'unknown'): Promise<{ game: Game, events: AnimationEvent[] }> => {
    const MAX_ATTEMPTS = 5;

    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        const loadedGame: Game = await loadCompleteGame(game_id);
        const expectedVersion = loadedGame.version ?? 0;

        // End-game race: if the game already ended (a concurrent move finished it
        // while this request was in flight — e.g. a human cover landing right as the
        // game-ending move commits), the incoming move is MOOT. Return the final
        // state instead of running the handler, which would throw "can't act in a
        // finished game" and surface as the client's "missing game ID" error. The
        // client just transitions to the win screen.
        if (loadedGame.status === GAME_STATUS.GAME_OVER) {
            console.log(`[${reqId}][TXN] game ${game_id} already over — move is a no-op`);
            return { game: loadedGame, events: [] };
        }

        // Snapshot card-conservation BEFORE this op so we can tell whether THIS
        // operation introduced a duplication or it was already corrupt on load.
        const beforeAudit = auditCards(loadedGame);

        const result = await operation(loadedGame);

        // Pure end-of-game detection: sets GAME_OVER + player statuses in memory,
        // no DB writes — so the committed state below is already final.
        const game_ended = check_win_sync(result.game);

        // Atomic, version-gated commit of the whole game state.
        const commit = await commitGame(result.game, expectedVersion);

        if (commit.status === 'conflict') {
            // Another actor committed between our load and our commit, so the move
            // we just computed is against stale state. Reload and redo it.
            await botDiag(game_id, 'T2_GAMELOCK', { event: 'cas_conflict', reqId, attempt });
            if (attempt < MAX_ATTEMPTS) continue;
            throw new Error(`Could not commit game ${game_id} after ${MAX_ATTEMPTS} attempts — write contention`);
        }

        // Card-conservation audit on the just-committed state. Log ONLY on a
        // violation (wrong total, or a duplicate (suit,value)). `brokeThisOp`
        // distinguishes corruption introduced by THIS move from pre-existing.
        const afterAudit = auditCards(result.game);
        if ((beforeAudit && !beforeAudit.ok) || (afterAudit && !afterAudit.ok)) {
            await botDiag(game_id, 'CARD_AUDIT', {
                reqId,
                brokeThisOp: !!(beforeAudit?.ok && afterAudit && !afterAudit.ok),
                status: result.game.status,
                pc: result.game.players.length,
                before: beforeAudit && { total: beforeAudit.total, expected: beforeAudit.expected, dups: beforeAudit.duplicates },
                after: afterAudit && { total: afterAudit.total, expected: afterAudit.expected, enumerated: afterAudit.enumerated, discard: afterAudit.discard, dups: afterAudit.duplicates },
            });
        }

        // Logs are append-only + UUID-keyed (idempotent), so they live outside the
        // CAS. For an ended game, finalizeEndedGame snapshots from the in-memory
        // logs and wipes the table, so we don't persist them here.
        if (!game_ended && result.game.logs.length > 0) {
            await saveGameLogs(supabaseClient, game_id, result.game.logs);
        }

        // End-of-game one-time side effects (ELO + replay snapshot + log wipe),
        // run exactly once — only the winning commit reaches here.
        if (game_ended) {
            await finalizeEndedGame(result.game);
            result.events.push({ type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION, game_state: result.game });
        }

        // Broadcast AFTER the durable commit (fire-and-forget).
        if (result.events.length > 0) {
            console.log(`[${reqId}][TXN] Broadcasting ${result.events.length} events after commit`);
            broadcastAnimationEvents(result.game, result.events, reqId).catch(err =>
                console.error(`[${reqId}] Error broadcasting events:`, err));
        }

        return result;
    }

    // The loop always returns or throws above; keeps the type checker happy.
    throw new Error(`Could not commit game ${game_id}`);
};

// Helper function to convert full Game to PublicGame (removes private data)
const gameToPublicGame = (game: Game): PublicGame => {
    return {
        id: game.id,
        name: game.name,
        deck_length: game.deck.length,
        discard_pile_length: game.discard_pile_length,
        flipped: game.flipped,
        players: game.players.map(other_player),
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        defender: game.defender,
        table_battles: game.table_battles,
        elimination_order: game.elimination_order,
        good_timestamp: game.good_timestamp,
        good_players: game.good_players,
    };
};

// Helper function to create PersonalGame from Game and player
const gameToPersonalGame = (game: Game, player: PrivatePlayer): PersonalGame => {
    return {
        ...gameToPublicGame(game),
        self: player
    };
};

// Helper function to create card backs for sanitization
const createCardBacks = (count: number): Card[] => {
    return Array(count).fill({ suit: -1, value: -1 });
};

// Helper function to check if event should have cards sanitized
const shouldSanitizeCards = (event: AnimationEvent): boolean => {
    return (event.type === ANIMATION_EVENT_TYPE.REFILL || event.type === ANIMATION_EVENT_TYPE.DEAL) && !!event.cards;
};

// Convert server AnimationEvents to PublicAnimationEvents for spectators
export const convertToPublicAnimationEvents = (events: AnimationEvent[]): PublicAnimationEvent[] => {
    return events.map(event => {
        const publicEvent: PublicAnimationEvent = { ...event };

        // Sanitize REFILL and DEAL events - hide all cards going to players' hands
        if (shouldSanitizeCards(event)) {
            publicEvent.cards = createCardBacks(event.cards!.length);
        }

        // Convert game_state from full Game to PublicGame
        if (event.game_state) {
            publicEvent.game_state = gameToPublicGame(event.game_state);
        }

        return publicEvent;
    });
};

// Convert server AnimationEvents to PersonalAnimationEvents for a specific player
export const convertToPersonalAnimationEvents = (events: AnimationEvent[], forPlayerId: string): PersonalAnimationEvent[] => {
    return events.map(event => {
        const { game_state, ...baseEvent } = event;

        const rawGameState: Game = event.game_state;

        if (!rawGameState) {
            throw new Error(`Game state not found in animation event, removing game_state from animation event`);
        }

        const baseGameState: PublicGame = gameToPublicGame(rawGameState);

        const playerSelf = rawGameState.players.find(p => p.player_id === forPlayerId);
        if (!playerSelf) {
            throw new Error(`Player ${forPlayerId} not found in game state, removing game_state from animation event`);
        }

        const personalGameState: PersonalGame = { ...baseGameState, self: playerSelf };

        if (shouldSanitizeCards(event) && event.player_id && event.player_id !== forPlayerId) {
            baseEvent.cards = createCardBacks(event.cards!.length);
        }

        return {
            ...baseEvent,
            game_state: personalGameState
        }
    });
};


// Broadcast animation events to all players and spectators
export const broadcastAnimationEvents = async (game: Game, events: AnimationEvent[], reqId: string = 'unknown'): Promise<void> => {
    if (events.length === 0) {
        return;
    }

    const broadcastTotalStart = Date.now();
    console.log(`[${reqId}][BROADCAST] broadcastAnimationEvents called for game ${game.id} with ${events.length} events`);

    try {
    // Calculate base game state once (shared for all players) - removes private information
    const baseGameStart = Date.now();
    const baseGameState = gameToPublicGame(game);
    console.log(`[${reqId}][BROADCAST] gameToPublicGame took ${Date.now() - baseGameStart}ms`);

    // Send personalized events to each player in parallel (excluding bots - they have no clients)
    const playerBroadcastStart = Date.now();
    const humanPlayers = game.players.filter(player => !player.is_ai);
    console.log(`[${reqId}][BROADCAST] Broadcasting to ${humanPlayers.length} human players (skipping ${game.players.length - humanPlayers.length} bots)`);
    
    const playerBroadcastPromises = humanPlayers.map(async (player) => {
        const playerStart = Date.now();
        const personalEvents = convertToPersonalAnimationEvents(events, player.player_id);

        const payload = {
            type: 'animation_sequence',
            events: personalEvents,
            sequence_id: crypto.randomUUID(),
            timestamp: Date.now()
        };

        console.log(`[${reqId}][BROADCAST] Sending ${personalEvents.length} events to ${player.name}`);

        // Create personalized game state by adding player's self data
        const personalizedGame = gameToPersonalGame(game, player);

        const channelCreateStart = Date.now();
        const channel = supabaseClient.channel(`gu-${game.id}-${player.player_id}`, {
            config: { private: true }
        });
        console.log(`[${reqId}][BROADCAST] Channel create for ${player.name} took ${Date.now() - channelCreateStart}ms`);

        const sendStart = Date.now();
        await channel.send({
            type: 'broadcast',
            event: 'animation_events',
            payload: {
                ...payload,
                game: personalizedGame
            }
        });
        console.log(`[${reqId}][BROADCAST] Channel send for ${player.name} took ${Date.now() - sendStart}ms`);

        const removeStart = Date.now();
        await supabaseClient.removeChannel(channel);
        console.log(`[${reqId}][BROADCAST] Channel remove for ${player.name} took ${Date.now() - removeStart}ms`);
        
        console.log(`[${reqId}][BROADCAST] Total for ${player.name}: ${Date.now() - playerStart}ms`);
    });
    
    // Wait for all player broadcasts to complete in parallel
    await Promise.all(playerBroadcastPromises);
    console.log(`[${reqId}][BROADCAST] All player broadcasts took ${Date.now() - playerBroadcastStart}ms`);

    // Send public events to spectator channel
    const spectatorStart = Date.now();
    const publicEvents = convertToPublicAnimationEvents(events);

    const spectatorPayload = {
        type: 'animation_sequence',
        events: publicEvents,
        sequence_id: crypto.randomUUID(),
        timestamp: Date.now()
    };

    console.log(`[${reqId}][BROADCAST] Sending ${publicEvents.length} events to spectators`);

    const spectatorChannel = supabaseClient.channel(`game-${game.id}`, {
        config: { private: true }
    });

    await spectatorChannel.send({
        type: 'broadcast',
        event: 'animation_events',
        payload: {
            ...spectatorPayload,
            game: baseGameState
        }
    });

    await supabaseClient.removeChannel(spectatorChannel);
    console.log(`[${reqId}][BROADCAST] Spectator broadcast took ${Date.now() - spectatorStart}ms`);
    
    const broadcastTotalMs = Date.now() - broadcastTotalStart;
    console.log(`[${reqId}][BROADCAST] Total broadcastAnimationEvents took ${broadcastTotalMs}ms`);
    // T5: a slow broadcast is a perceived freeze — the move IS applied server-side
    // but no client renders it until this resolves (or a manual refetch "snaps" it).
    if (broadcastTotalMs > 2000) {
        await botDiag(game.id, 'T5_BROADCAST', {
            event: 'slow', reqId, totalMs: broadcastTotalMs,
            events: events.length,
            humanPlayers: game.players.filter(p => !p.is_ai).length,
        });
    }
    } catch (err: any) {
        // T5: broadcast threw — clients get NOTHING for this move and the board looks
        // stuck until the next event/refetch. Normally swallowed by the callers'
        // fire-and-forget .catch, so capture it here before rethrowing.
        await botDiag(game.id, 'T5_BROADCAST', {
            event: 'error', reqId, message: String(err?.message ?? err),
            events: events.length,
        });
        throw err;
    }
};

export interface ExecutionParams {
    user: User;
    user_name: string;
    body: any;
    game: Game;
    reqId: string;
}

export const wrap400 = (execute: (params: ExecutionParams) => Promise<{ game: Game, events: AnimationEvent[] }>, run_bots: boolean = false) => {
    const handler = async (req: Request): Promise<Response> => {
        // Generate unique request ID (short hash from crypto)
        const reqId = crypto.randomUUID().split('-')[0];
        const requestStartTime = Date.now();
        console.log(`[${reqId}][WRAP400] ========== REQUEST START: ${req.method} ${req.url} ==========`);
        
        try {
            // Handle CORS
            const corsStart = Date.now();
            const corsResponse = handleCors(req);
            console.log(`[${reqId}][WRAP400] CORS check took ${Date.now() - corsStart}ms`);
            if (corsResponse) {
                console.log(`[${reqId}][WRAP400] Returning CORS response (total: ${Date.now() - requestStartTime}ms)`);
                return corsResponse;
            }

            // Get authenticated user
            const authStart = Date.now();
            const user: User = await getAuthenticatedUser(req);
            console.log(`[${reqId}][WRAP400] Authentication took ${Date.now() - authStart}ms`);

            // Get user name from email
            const user_name = user.user_metadata.username;

            // Parse JSON body
            const bodyParseStart = Date.now();
            let body = {};
            try {
                body = await req.json();
            } catch (e) { }
            console.log(`[${reqId}][WRAP400] Body parsing took ${Date.now() - bodyParseStart}ms`);
            // If JSON parsing fails, keep empty object

            // Extract game_id from body for lock management
            const game_id = (body as any).game_id;
            console.log(`[${reqId}][WRAP400] game_id: ${game_id || 'none'}`);

            let result: any;
            let events: AnimationEvent[] = [];

            if (game_id) {
                // Execute operation with database lock for this specific game
                const lockStart = Date.now();
                console.log(`[${reqId}][WRAP400] Starting executeWithGameLock for game ${game_id}`);
                const { game, events: operationEvents } = await executeWithGameLock(game_id, (game) => execute({ user, user_name, body, game, reqId }), reqId);
                console.log(`[${reqId}][WRAP400] executeWithGameLock took ${Date.now() - lockStart}ms`);
                result = game;
                events = operationEvents;
            } else {
                // No game_id, execute immediately (for operations that don't involve games)
                // pretty much only create
                const executeStart = Date.now();
                console.log(`[${reqId}][WRAP400] Starting direct execute (no game_id)`);
                const operationResult = await execute({ user, user_name, body, game: {} as Game, reqId });
                console.log(`[${reqId}][WRAP400] Direct execute took ${Date.now() - executeStart}ms`);

                result = operationResult.game;
                events = operationResult.events;
            }

            // handle spectating here too 
            const personalizeStart = Date.now();
            const personalized_result = personalize_game(result, user.id);
            console.log(`[${reqId}][WRAP400] personalize_game took ${Date.now() - personalizeStart}ms`);

            // Note: Animation events are now broadcasted automatically by executeWithGameLock
            // for game_id operations. For game creation (no game_id), we still need to broadcast here.
            if (!game_id && events.length > 0 && result && result.id) {
                console.log(`[${reqId}][WRAP400] Starting fire-and-forget broadcast for game creation (${events.length} events)`);
                broadcastAnimationEvents(result, events, reqId).catch(err => 
                    console.error(`[${reqId}] Background broadcast error:`, err)
                );
            }

            // Background bot loop, scheduled AFTER preparing the response (non-blocking).
            if (game_id && run_bots) {
                console.log(`[${reqId}][WRAP400] Starting background bot loop`);
                // TODO: not quite. Only after start/attack/cover/pass/pickup/good
                // todo add validation before kicking this off
                //
                // CRITICAL: this runs AFTER the HTTP response is sent. Without
                // EdgeRuntime.waitUntil the runtime reaps the isolate ~15s later —
                // mid-loop — so lockedBotLoop's `finally` never releases the bot_locks
                // baton and it leaks for the full stale window, freezing the game
                // (confirmed by T1 diagnostics: loops died at ~15s with no
                // released_clean). waitUntil keeps the worker alive until it settles.
                const botLoop = lockedBotLoop(game_id).catch(err =>
                    console.error(`[${reqId}] bot loop error:`, err));
                const er = (globalThis as any).EdgeRuntime;
                if (er && typeof er.waitUntil === 'function') {
                    er.waitUntil(botLoop);
                }
            }

            // Create standardized response and return immediately
            const responseTime = Date.now() - requestStartTime;
            console.log(`[${reqId}][WRAP400] ========== RETURNING RESPONSE (total: ${responseTime}ms) ==========`);
            return new Response(JSON.stringify(personalized_result), {
                headers: {
                    ...corsHeaders,
                    'Content-Type': 'application/json'
                }
            });
        } catch (e: any) {
            const errorTime = Date.now() - requestStartTime;
            console.error(`[${reqId}][WRAP400] Error processing request (after ${errorTime}ms):`, {
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
    };

    // Serve the handler
    serve(handler);

    // Return the handler (though it won't be used after serve() is called)
    return handler;
};

// =============================================================================
// DATABASE HELPER FUNCTIONS FOR SEPARATED SCHEMA - Using JOINs
// =============================================================================

// Load complete game state from separated tables using efficient JOINs
export const loadCompleteGame = async (game_id: string): Promise<Game> => {
    //converter of SQL to useful game object
    // Use JOIN to get all data in one query
    const { data, error } = await supabaseClient
        .from('games')
        .select(`
            *,
            game_decks(deck),
            player_hands(player_id, hand, awaiting_attack),
            bot_hands(bot_id, hand, awaiting_attack, bots(strategy_key)),
            game_logs(id, game_id, log_type, player_id, card_pairs, defender_index, created_at)
        `)
        .eq('id', game_id)
        .single();

    if (error) {
        console.error('Error loading complete game', error);
        throw new Error(`Game ${game_id} not found`);
    }

    console.log(JSON.stringify(data));

    const players: PrivatePlayer[] = data.players.map((player: any) => {
        let hand, awaiting_attack, strategy_key;

        if (player.is_ai) {
            // Look up in bot_hands table
            const botHand = data.bot_hands.find(hand => hand.bot_id === player.player_id)!;
            if (botHand) {
                hand = botHand.hand;
                awaiting_attack = botHand.awaiting_attack;
                strategy_key = botHand.bots.strategy_key;
            }
        } else {
            // Look up in player_hands table
            const playerHand = data.player_hands.find(hand => hand.player_id === player.player_id)!;
            hand = playerHand.hand;
            awaiting_attack = playerHand.awaiting_attack;
            strategy_key = STRATEGY_KEY.HUMAN;
        }

        return {
            player_id: player.player_id,
            name: player.name,
            status: player.status,
            is_ai: player.is_ai,
            hand: hand,
            awaiting_attack: awaiting_attack,
            hand_length: hand.length,
            strategy_key: strategy_key,
        } as PrivatePlayer;
    });

    // Filter logs to get only the current game session
    // Find the most recent GAME_START and return all logs after it
    let logs: GameLog[] = [];
    if (data.game_logs && data.game_logs.length > 0) {
        const allLogs = data.game_logs.sort((a: any, b: any) => 
            new Date(a.created_at).getTime() - new Date(b.created_at).getTime()
        );
        
        // Find the index of the most recent GAME_START
        let gameStartIndex = -1;
        for (let i = allLogs.length - 1; i >= 0; i--) {
            if (allLogs[i].log_type === 'game_start') {
                gameStartIndex = i;
                break;
            }
        }
        
        // If GAME_START found, return logs from that point onwards
        if (gameStartIndex !== -1) {
            logs = allLogs.slice(gameStartIndex);
        }
    }

    const game: Game = {
        id: data.id,
        version: data.version ?? 0, // optimistic-concurrency token for commit_game
        name: data.name,
        deck: data.game_decks.deck,
        // unused but necessary for type
        deck_length: data.game_decks.deck.length,
        discard_pile_length: data.discard_pile_length,
        flipped: data.flipped,
        players: players,
        status: data.status,
        power_suit: data.power_suit,
        first_attacker: data.first_attacker,
        defender: data.defender,
        table_battles: data.table_battles,
        elimination_order: data.elimination_order,
        good_timestamp: data.good_timestamp || null,
        good_players: data.good_players || [],
        logs: logs, // Loaded from database
    }

    return game;
    /*

    data looks like:
    {
  "id": "9c4b97",
  "name": "alex603's Game",
  "flipped": null,
  "players": [
    {
      "id": "72c6ac7d-5017-4ff6-86f8-df411d7035dd",
      "name": "alex603",
      "status": "idle"
    }
  ],
  "status": "waiting",
  "power_suit": 0,
  "first_attacker": 0,
  "defender": 0,
  "table_battles": [],
  "created_at": "2025-07-01T06:51:13.74066+00:00",
  "updated_at": "2025-07-01T06:51:13.74066+00:00",
  "game_decks": {
    "deck": []
  },
  "player_hands": [
    {
      "hand": [],
      "player_id": "72c6ac7d-5017-4ff6-86f8-df411d7035dd"
      "awaiting_attack": false,
    }
  ]
}
    */
};

// Atomically commit the full game state via the commit_game RPC, gated on the
// version we loaded (optimistic concurrency — replaces the old game_locks +
// multi-statement saveCompleteGame). One DB transaction across games/game_decks/
// player_hands/bot_hands → no torn reads; the version gate is the fence that makes
// double-apply impossible. Returns 'conflict' if another writer committed first;
// the caller reloads and retries. Logs are handled separately (idempotent).
export const commitGame = async (
    game: Game,
    expectedVersion: number,
): Promise<{ status: 'ok' | 'conflict'; version?: number }> => {
    const publicGame: PublicGame = gameToPublicGame(game);

    const humanHands = game.players
        .filter(player => !player.is_ai)
        .map(player => ({
            player_id: player.player_id,
            hand: player.hand,
            awaiting_attack: player.awaiting_attack,
        }));

    const botHands = game.players
        .filter(player => player.is_ai)
        .map(player => ({
            bot_id: player.player_id,
            hand: player.hand,
            awaiting_attack: player.awaiting_attack,
        }));

    const { data, error } = await supabaseClient.rpc('commit_game', {
        p_game_id: game.id,
        p_expected_version: expectedVersion,
        p_game: publicGame,
        p_deck: game.deck,
        p_hands: humanHands,
        p_bot_hands: botHands,
    });

    if (error) {
        console.error(`[COMMIT] commit_game RPC failed for ${game.id}:`, error);
        throw error;
    }

    const res = data as { status: 'ok' | 'conflict'; version?: number };
    if (res.status === 'ok' && typeof res.version === 'number') {
        game.version = res.version; // keep the in-memory game's token current
    }
    return res;
};

// Get player's hand for a specific game using direct query
export const getPlayerHand = async (game_id: string, player_id: string): Promise<Card[]> => {
    const { data, error } = await supabaseClient
        .from('player_hands')
        .select('hand')
        .eq('game_id', game_id)
        .eq('player_id', player_id)
        .single();

    if (error) {
        console.error('Error loading player hand', error);
        return [];
    }

    return data?.hand || [];
};

// Update player's hand efficiently
export const updatePlayerHand = async (game_id: string, player_id: string, hand: Card[]): Promise<void> => {
    await supabaseClient
        .from('player_hands')
        .upsert({
            game_id: game_id,
            player_id: player_id,
            hand: hand
        });
};

// =============================================================================
// REALTIME BROADCAST UTILITIES
// =============================================================================

// private message to specific user
export const broadcastToGameUser = async (game: Game, messageType: string, baseMessage: any, user_id: string): Promise<void> => {
    const channel = supabaseClient.channel(`gu-${game.id}-${user_id}`, {
        config: { private: true }
    });
    await channel.send({
        type: 'broadcast',
        event: messageType,
        payload: {
            ...baseMessage,
            game: personalize_game(game, user_id)
        }
    });
    await supabaseClient.removeChannel(channel);
}


// Functions moved to common_utils.ts

// true if game is over
const check_win_sync = (game: Game): boolean => {
    const the_fool = game_done(game);
    if (the_fool === null) {
        return false
    }
    // Set game status to GAME_OVER to show win screen
    game.status = GAME_STATUS.GAME_OVER;

    // set all players to idle but keep their hands for display
    game.players.forEach((player: PrivatePlayer) => {
        if (player.is_ai) {
            player.status = PLAYER_STATUS.READY;
        } else {
            player.status = PLAYER_STATUS.IDLE;
        }
    });

    return true;
}

// One-time end-of-game side effects (ELO + replay snapshot + log wipe). Run by
// executeWithGameLock AFTER the final GAME_OVER state is durably committed, so it
// fires exactly once (only the winning CAS commit reaches it). This is the tail of
// the old check_win_async; its check_win_sync half now runs BEFORE the commit so
// the committed state already reflects GAME_OVER.
const finalizeEndedGame = async (game: Game): Promise<void> => {
    // Update ELO ratings
    await updateEloRatings(game);

    // Compress the finished session into a replay snapshot (game_snapshots
    // row) and retire its logs. verifyRoundTrip both encodes and proves the
    // encoding decodes back to the exact action sequence — only on success do
    // we touch the logs. Clearing game.logs afterwards stops saveCompleteGame
    // from re-inserting the rows we just wiped.
    try {
        const { encoded } = verifyRoundTrip({
            playerIds: game.players.map(player => player.player_id),
            logs: game.logs,
            flipped: game.flipped,
        });

        // Stored binary: `moves` is the rANS integer (the whole game),
        // `extras` the names + timing blob (_shared/replay/extras.ts). The
        // share code is derived client-side: base32(moves) ['-' base32(extras)].
        // One row per finished session in game_snapshots; player_ids is the
        // read ACL.
        const extrasBytes = base32Decode(encodeExtras(
            game.players.map(player => player.name),
            moveTimesFromLogs(game.logs),
        ));
        console.log(`[REPLAY] Game ${game.id} encoded to ${encoded.byteLength}+${extrasBytes.length} bytes`);

        // Persist the snapshot BEFORE destroying the logs, so a failure
        // between the two never loses both.
        const { error: snapError } = await supabaseClient
            .from('game_snapshots')
            .insert({
                game_id: game.id,
                player_ids: game.players.map(player => player.player_id),
                moves: bytesToHex(encoded.bytes),
                extras: bytesToHex(extrasBytes),
            });
        if (snapError) throw snapError;

        await wipeAllGameLogs(supabaseClient, game.id);
        game.logs = [];
    } catch (error) {
        // Never break game completion over the snapshot; keep the logs as the
        // fallback record and fall back to the age-based cleanup.
        console.error(`[REPLAY] Snapshot failed for game ${game.id} — keeping logs:`, error);
        cleanupOldGameLogs(supabaseClient, game.id).catch(err => {
            console.error(`Error cleaning up old logs for game ${game.id}:`, err);
        });
    }
}


// =============================================================================
// ELO RATING SYSTEM
// =============================================================================



// Get or create ELO rating for a user
export const getOrCreateEloRating = async (userId: string): Promise<UserEloRating> => {
    const { data, error } = await supabaseClient
        .from('user_elo_ratings')
        .select('*')
        .eq('user_id', userId)
        .single();

    if (error && error.code === 'PGRST116') {
        // User doesn't have ELO rating, create one
        const newRating = {
            user_id: userId,
            elo_rating: 1000,
            games_played: 0
        };

        const { data: insertData, error: insertError } = await supabaseClient
            .from('user_elo_ratings')
            .insert(newRating)
            .select()
            .single();

        if (insertError) {
            console.error('Error creating ELO rating:', insertError);
            throw new Error('Failed to create ELO rating');
        }

        return insertData;
    }

    if (error) {
        console.error('Error fetching ELO rating:', error);
        throw new Error('Failed to fetch ELO rating');
    }

    return data;
};

// Get ELO rating for a bot
export const getBotEloRating = async (botId: string): Promise<{ elo_rating: number, games_played: number, nickname: string, strategy_key: string }> => {
    const { data, error } = await supabaseClient
        .from('bots')
        .select('elo_rating, games_played, nickname, strategy_key')
        .eq('id', botId)
        .single();

    if (error) {
        console.error('Error fetching bot ELO rating:', error);
        throw new Error('Failed to fetch bot ELO rating');
    }

    return data;
};

// Update ELO ratings for all players after game completion
export const updateEloRatings = async (game: Game): Promise<void> => {
    if (game.players.length < 2) {
        return; // No ELO updates for single player games
    }

    try {
        // Get all player ELO ratings (both human and bot)
        const playerRatings = new Map<string, { elo_rating: number, games_played: number }>();
        const botData = new Map<string, { elo_rating: number, games_played: number, nickname: string, strategy_key: string }>();
        const humanPlayers: string[] = [];
        const botPlayers: string[] = [];

        for (const player of game.players) {
            if (player.is_ai) {
                const botInfo = await getBotEloRating(player.player_id);
                playerRatings.set(player.player_id, botInfo);
                botData.set(player.player_id, botInfo);
                botPlayers.push(player.player_id);
            } else {
                const rating = await getOrCreateEloRating(player.player_id);
                playerRatings.set(player.player_id, rating);
                humanPlayers.push(player.player_id);
            }
        }

        // Determine final rankings based on elimination order
        const rankings = calculateGameRankings(game);

        // Safety check: ensure all players are in rankings
        if (rankings.length !== game.players.length) {
            console.error(`ERROR: Rankings incomplete! Expected ${game.players.length} players, got ${rankings.length} in rankings`);
            console.error('This will cause incorrect ELO calculations. Skipping ELO update.');
            return;
        }

        // Calculate ELO changes for each player
        const ratingChanges = new Map<string, number>();

        for (let i = 0; i < rankings.length; i++) {
            const playerId = rankings[i];
            const playerRating = playerRatings.get(playerId)!;
            let totalChange = 0;

            // For each other player, calculate 1v1 ELO change
            for (let j = 0; j < rankings.length; j++) {
                if (i === j) continue;

                const opponentId = rankings[j];
                const opponentRating = playerRatings.get(opponentId)!;

                // Determine score: 1 if player finished better, 0 if worse, 0.5 if tie
                let score: number;
                if (i < j) {
                    score = 1; // Player finished better
                } else if (i > j) {
                    score = 0; // Player finished worse
                } else {
                    score = 0.5; // Tie (shouldn't happen in our ranking system)
                }

                const change = calculateEloChange(playerRating.elo_rating, opponentRating.elo_rating, score);
                totalChange += change;
            }

            ratingChanges.set(playerId, totalChange);
        }

        // Update human player ratings
        const humanRatingUpdates: Array<{ user_id: string, elo_rating: number, previous_elo: number, games_played: number }> = [];
        for (const playerId of humanPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings

            humanRatingUpdates.push({
                user_id: playerId,
                elo_rating: newRating,
                previous_elo: currentRating.elo_rating, // Store previous ELO
                games_played: currentRating.games_played + 1
            });
        }

        if (humanRatingUpdates.length > 0) {
            await supabaseClient
                .from('user_elo_ratings')
                .upsert(humanRatingUpdates);
        }

        // Update bot ratings
        const botRatingUpdates: Array<{ id: string, nickname: string, strategy_key: string, elo_rating: number, previous_elo: number, games_played: number }> = [];
        for (const playerId of botPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const currentBotData = botData.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings

            botRatingUpdates.push({
                id: playerId,
                nickname: currentBotData.nickname,
                strategy_key: currentBotData.strategy_key,
                elo_rating: newRating,
                previous_elo: currentRating.elo_rating,
                games_played: currentRating.games_played + 1
            });
        }

        if (botRatingUpdates.length > 0) {
            const { data: botUpdateData, error: botUpdateError } = await supabaseClient
                .from('bots')
                .upsert(botRatingUpdates)
                .select();

            if (botUpdateError) {
                console.error('Error updating bot ratings:', botUpdateError);
            } else {
            }
        }

    } catch (error) {
        console.error('Error updating ELO ratings:', error);
        // Don't throw error to prevent breaking game completion
    }
};
