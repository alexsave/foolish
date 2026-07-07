import { corsHeaders, handleCors } from './cors.ts';
import {
    personalize_game,
    calculateEloChange,
    calculateGameRankings,
    game_done,
    other_player,
} from './common_utils.ts';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PersonalGame, PrivatePlayer, PublicGame, PlayerHand, UserEloRating, BotHand, AnimationEvent, PublicAnimationEvent, PersonalAnimationEvent, ANIMATION_EVENT_TYPE, GameLog, STRATEGY_KEY } from './types.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import type { User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';
import { cleanupOldGameLogs, wipeAllGameLogs, loadCurrentSessionLogs } from './log_utils.ts';
// NOTE: bot_actions (→ the entire bot-strategy stack: cordite's ~127KB Monte-Carlo
// engine, etc.) and the replay codec are imported LAZILY at their use sites
// below, NOT statically. wrap400 is imported by every edge function, so a static
// import here would make cold starts of lightweight functions (create/join/start/
// the lobby) transpile+evaluate that whole graph for nothing — the dominant cost
// behind multi-second "create game" cold starts. They now load only when a bot
// actually drives (the run_bots branch) or a game actually ends (finalizeEndedGame).

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// (game_locks acquire/release removed — concurrency is now optimistic CAS via
// the commit_game RPC; see executeWithGameLock below.)

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
export const executeWithGameLock = async (game_id: string, operation: (game: Game) => Promise<{ game: Game, events: AnimationEvent[], deleted?: boolean }>, reqId: string = 'unknown', mootIfGameOver: boolean = false): Promise<{ game: Game, events: AnimationEvent[], deleted?: boolean }> => {
    const MAX_ATTEMPTS = 5;

    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        const loadedGame: Game = await loadCompleteGame(game_id);
        const expectedVersion = loadedGame.version ?? 0;

        // End-game race: for a MOVE (mootIfGameOver), if the game already ended (a
        // concurrent move finished it while this request was in flight — e.g. a human
        // cover landing right as the game-ending move commits), the incoming move is
        // MOOT. Return the final state instead of running the handler, which would
        // throw "can't act in a finished game" and surface as the client's "missing
        // game ID" error. NOT applied to post-game actions like `continue` (reset to
        // lobby), which legitimately operate on a GAME_OVER game.
        if (mootIfGameOver && loadedGame.status === GAME_STATUS.GAME_OVER) {
            console.log(`[${reqId}][TXN] game ${game_id} already over — move is a no-op`);
            return { game: loadedGame, events: [] };
        }

        const result = await operation(loadedGame);

        // The operation deleted the game row itself (last player exiting the
        // lobby). There is nothing left to CAS against — committing would report
        // a spurious `conflict`, and the retry's reload would throw "not found",
        // turning a successful teardown into a 400.
        if (result.deleted) {
            console.log(`[${reqId}][TXN] game ${game_id} deleted by operation — skipping commit`);
            return result;
        }

        // Pure end-of-game detection: sets GAME_OVER + player statuses in memory,
        // no DB writes — so the committed state below is already final.
        const game_ended = check_win_sync(result.game);

        // Atomic, version-gated commit of the whole game state.
        const commit = await commitGame(result.game, expectedVersion);

        if (commit.status === 'conflict') {
            // Another actor committed between our load and our commit, so the move
            // we just computed is against stale state. Reload and redo it.
            if (attempt < MAX_ATTEMPTS) continue;
            throw new Error(`Could not commit game ${game_id} after ${MAX_ATTEMPTS} attempts — write contention`);
        }

        // End-of-game one-time side effects (ELO + replay snapshot + log wipe),
        // run exactly once — only the winning commit reaches here. This MUST run
        // before the broadcast: it pushes the MAGIC_TRANSITION event and the final
        // state the broadcast carries. The ending move's own logs were committed
        // atomically with the final state above, so finalizeEndedGame loads the
        // COMPLETE session for the replay snapshot.
        if (game_ended) {
            await finalizeEndedGame(result.game);
            result.events.push({ type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION, game_state: result.game });
        }

        // Broadcast AFTER the durable commit (fire-and-forget). The move's logs
        // were part of the commit itself (commit_game p_logs) — there is no
        // separate log write left on this path; the fence is the version.
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
const convertToPublicAnimationEvents = (events: AnimationEvent[]): PublicAnimationEvent[] => {
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
const convertToPersonalAnimationEvents = (events: AnimationEvent[], forPlayerId: string): PersonalAnimationEvent[] => {
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
// One batched REST broadcast for many topics in a single POST.
//
// This is exactly what realtime-js's httpSend() does internally (see
// node_modules/@supabase/realtime-js/.../RealtimeChannel.js) — a POST of
// { messages: [{ topic, event, payload, private }] } to /realtime/v1/api/broadcast
// — generalized to many topics so ALL recipients of one game update go out in a
// SINGLE round-trip. The old path created a channel per recipient and called
// channel.send(), which (the channel was never subscribed) silently fell back to
// this same REST endpoint ONE connection at a time, plus a removeChannel and the
// "Realtime send() is automatically falling back to REST API ... use httpSend()"
// deprecation warning per call. The server never holds a websocket; clients keep
// receiving over their existing subscriptions. `topic` is the bare channel name
// (realtime-js strips the "realtime:" prefix to form subTopic); all our channels
// are private, hence private: true.
const REALTIME_BROADCAST_URL = `${Deno.env.get('SUPABASE_URL') || ''}/realtime/v1/api/broadcast`;
const REALTIME_BROADCAST_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';

interface BroadcastMessage { topic: string; event: string; payload: any; }

const broadcastMessages = async (messages: BroadcastMessage[], reqId: string = 'unknown'): Promise<void> => {
    if (messages.length === 0) return;
    const start = Date.now();
    // One retry on failure: the whole path is fire-and-forget (a dropped
    // broadcast only surfaces as a missed animation until the next event's
    // versioned state supersedes it), so a single cheap re-send covers the
    // transient Realtime hiccup without adding meaningful tail latency.
    // Duplicate delivery is safe — clients dedup by sequence_id and drop
    // stale versions.
    for (let attempt = 1; attempt <= 2; attempt++) {
        try {
            const response = await fetch(REALTIME_BROADCAST_URL, {
                method: 'POST',
                headers: {
                    apikey: REALTIME_BROADCAST_KEY,
                    Authorization: `Bearer ${REALTIME_BROADCAST_KEY}`,
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    messages: messages.map(m => ({
                        topic: m.topic,
                        event: m.event,
                        payload: m.payload,
                        private: true,
                    })),
                }),
            });
            // Realtime returns 202 Accepted on success.
            if (response.status === 202) {
                await response.body?.cancel();
                break;
            }
            const text = await response.text().catch(() => response.statusText);
            console.error(`[${reqId}][BROADCAST] REST broadcast failed (attempt ${attempt}): ${response.status} ${text}`);
        } catch (err) {
            console.error(`[${reqId}][BROADCAST] REST broadcast error (attempt ${attempt}):`, err);
        }
    }
    console.log(`[${reqId}][BROADCAST] batched ${messages.length} message(s) in ${Date.now() - start}ms`);
};

export const broadcastAnimationEvents = async (game: Game, events: AnimationEvent[], reqId: string = 'unknown'): Promise<void> => {
    if (events.length === 0) {
        return;
    }

    const broadcastTotalStart = Date.now();
    console.log(`[${reqId}][BROADCAST] broadcastAnimationEvents called for game ${game.id} with ${events.length} events`);

    // Calculate base game state once (shared for all players) - removes private information
    const baseGameStart = Date.now();
    const baseGameState = gameToPublicGame(game);
    console.log(`[${reqId}][BROADCAST] gameToPublicGame took ${Date.now() - baseGameStart}ms`);

    // Build one message per recipient, then fire them all in a single batched
    // POST (players are personalized; bots are skipped — they have no clients).
    const humanPlayers = game.players.filter(player => !player.is_ai);
    console.log(`[${reqId}][BROADCAST] Broadcasting to ${humanPlayers.length} human players + spectators (skipping ${game.players.length - humanPlayers.length} bots)`);

    const messages: BroadcastMessage[] = humanPlayers.map(player => ({
        topic: `gu-${game.id}-${player.player_id}`,
        event: 'animation_events',
        payload: {
            type: 'animation_sequence',
            events: convertToPersonalAnimationEvents(events, player.player_id),
            sequence_id: crypto.randomUUID(),
            timestamp: Date.now(),
            // Committed games.version this sequence reflects. Broadcasts can arrive
            // out of order under realtime latency; the client uses this monotonic
            // token to drop any sequence at or below the newest it has already
            // applied (no rubber-band).
            version: game.version ?? 0,
            game: gameToPersonalGame(game, player),
        },
    }));

    // Public events to the spectator channel.
    messages.push({
        topic: `game-${game.id}`,
        event: 'animation_events',
        payload: {
            type: 'animation_sequence',
            events: convertToPublicAnimationEvents(events),
            sequence_id: crypto.randomUUID(),
            timestamp: Date.now(),
            version: game.version ?? 0,
            game: baseGameState,
        },
    });

    await broadcastMessages(messages, reqId);
    console.log(`[${reqId}][BROADCAST] Total broadcastAnimationEvents took ${Date.now() - broadcastTotalStart}ms`);
};

export interface ExecutionParams {
    user: User;
    user_name: string;
    body: any;
    game: Game;
    reqId: string;
}

export const wrap400 = (execute: (params: ExecutionParams) => Promise<{ game: Game, events: AnimationEvent[] }>, run_bots: boolean = false, mootIfGameOver: boolean = false) => {
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
                const { game, events: operationEvents } = await executeWithGameLock(game_id, (game) => execute({ user, user_name, body, game, reqId }), reqId, mootIfGameOver);
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
            // Gated on the game actually PLAYING: a lobby action, a deleted game, or a
            // just-finished game has no bot to drive, and `bump` (membership-free by
            // design, spectators nudge stalled games) shouldn't be able to spin the
            // loop on non-live games either.
            if (game_id && run_bots && result?.status === GAME_STATUS.PLAYING) {
                try {
                    const m = (globalThis as { Deno?: { memoryUsage?: () => { rss: number; heapTotal: number; heapUsed: number; external: number } } }).Deno?.memoryUsage?.();
                    if (m) console.log(`[${reqId}][MEM] pre-loop: heap=${Math.round(m.heapUsed / 1048576)}/${Math.round(m.heapTotal / 1048576)}MB ext=${Math.round(m.external / 1048576)}MB rss=${Math.round(m.rss / 1048576)}MB`);
                } catch { /* memoryUsage unavailable */ }
                console.log(`[${reqId}][WRAP400] Starting background bot loop`);
                //
                // CRITICAL: this runs AFTER the HTTP response is sent. Without
                // EdgeRuntime.waitUntil the runtime reaps the isolate ~15s later —
                // mid-loop — so lockedBotLoop's `finally` never releases the bot_locks
                // baton and it leaks for the full stale window, freezing the game
                // (confirmed by T1 diagnostics: loops died at ~15s with no
                // released_clean). waitUntil keeps the worker alive until it settles.
                // Lazy import: only now (a real bot drive) do we pull in the bot
                // strategy stack. Lightweight functions never load it.
                const botLoop = import('./bot_actions.ts')
                    .then(m => m.lockedBotLoop(game_id))
                    .catch(err => console.error(`[${reqId}] bot loop error:`, err));
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
            bot_hands(bot_id, hand, awaiting_attack, bots(strategy_key))
        `)
        .eq('id', game_id)
        .single();

    if (error) {
        console.error('Error loading complete game', error);
        throw new Error(`Game ${game_id} not found`);
    }

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

    // Logs are loaded LAZILY (not here). Game logic never reads historical logs —
    // handlers only APPEND via addLog, and the per-move response/broadcast strip
    // logs entirely (gameToPublicGame / personalize_game). The only consumer of the
    // full session is the end-of-game replay snapshot, which loads it on demand
    // (finalizeEndedGame → loadCurrentSessionLogs). So every move starts with an
    // empty log buffer that collects just this move's new logs; we no longer pull
    // and sort the entire (growing) log history on the hot path.
    const logs: GameLog[] = [];

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

    // This move's logs ride in the SAME transaction as the version-gated
    // commit (migration 20260702100000): one round-trip instead of a commit +
    // separate saveGameLogs, and the logs can never be durably out of sync
    // with the state they describe. The 10-minute guard mirrors the old
    // saveGameLogs filter: anything older was loaded from the DB, not
    // produced by this move.
    const tenMinutesAgo = Date.now() - 10 * 60 * 1000;
    const freshLogs = game.logs.filter(log => new Date(log.created_at).getTime() > tenMinutesAgo)
        .map(log => ({
            id: log.id,
            game_id: log.game_id,
            log_type: log.log_type,
            player_id: log.player_id,
            card_pairs: log.card_pairs,
            defender_index: log.defender_index,
            created_at: log.created_at,
        }));

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
        p_logs: freshLogs.length > 0 ? freshLogs : null,
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



// =============================================================================
// REALTIME BROADCAST UTILITIES
// =============================================================================

// private message to specific user
export const broadcastToGameUser = async (game: Game, messageType: string, baseMessage: any, user_id: string): Promise<void> => {
    await broadcastMessages([{
        topic: `gu-${game.id}-${user_id}`,
        event: messageType,
        payload: {
            ...baseMessage,
            game: personalize_game(game, user_id)
        }
    }]);
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
    // ELO and the replay snapshot touch disjoint tables and don't read each
    // other's writes — run them concurrently instead of serially, since both
    // sit on the game-end critical path before the final broadcast.
    // updateEloRatings never throws (it swallows its own errors).
    const eloPromise = updateEloRatings(game);

    // Logs are loaded lazily, so game.logs holds only the FINAL move's logs here.
    // The replay snapshot needs the whole session, so load it from the DB (the
    // caller persisted this move's logs first, so the DB is complete). Fall back to
    // the in-memory logs if the load returns nothing, so a transient read failure
    // can't lose a snapshot it could otherwise still partially make.
    const sessionLogs = await loadCurrentSessionLogs(supabaseClient, game.id);
    const replayLogs = sessionLogs.length > 0 ? sessionLogs : game.logs;

    // Compress the finished session into a replay snapshot (game_snapshots
    // row) and retire its logs. verifyRoundTrip both encodes and proves the
    // encoding decodes back to the exact action sequence — only on success do
    // we touch the logs. Clearing game.logs afterwards stops saveCompleteGame
    // from re-inserting the rows we just wiped.
    try {
        // Lazy import: the replay codec is only needed here, at game end.
        const { verifyRoundTrip } = await import('./replay/encode.ts');
        const { encodeExtras, moveTimesFromLogs } = await import('./replay/extras.ts');
        const { base32Decode, bytesToHex } = await import('./replay/codec.ts');

        const { encoded } = await verifyRoundTrip({
            playerIds: game.players.map(player => player.player_id),
            logs: replayLogs,
            flipped: game.flipped,
        });

        // Stored binary: `moves` is the rANS integer (the whole game),
        // `extras` the names + timing blob (_shared/replay/extras.ts). The
        // share code is derived client-side: base32(moves) ['-' base32(extras)].
        // One row per finished session in game_snapshots; player_ids is the
        // read ACL.
        const extrasBytes = base32Decode(encodeExtras(
            game.players.map(player => player.name),
            moveTimesFromLogs(replayLogs),
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

    await eloPromise;
}


// =============================================================================
// ELO RATING SYSTEM
// =============================================================================



// Update ELO ratings for all players after game completion
const updateEloRatings = async (game: Game): Promise<void> => {
    if (game.players.length < 2) {
        return; // No ELO updates for single player games
    }

    try {
        // Load all player ELO ratings in TWO batched selects (one per table)
        // instead of one round-trip per player — this runs on the game-ending
        // hot path, before the final broadcast is pushed.
        const playerRatings = new Map<string, { elo_rating: number, games_played: number }>();
        const botData = new Map<string, { elo_rating: number, games_played: number, nickname: string, strategy_key: string }>();
        const humanPlayers: string[] = game.players.filter(p => !p.is_ai).map(p => p.player_id);
        const botPlayers: string[] = game.players.filter(p => p.is_ai).map(p => p.player_id);

        if (botPlayers.length > 0) {
            const { data, error } = await supabaseClient
                .from('bots')
                .select('id, elo_rating, games_played, nickname, strategy_key')
                .in('id', botPlayers);
            if (error || !data || data.length !== botPlayers.length) {
                console.error('Error fetching bot ELO ratings:', error ?? `expected ${botPlayers.length} bots, got ${data?.length ?? 0}`);
                throw new Error('Failed to fetch bot ELO ratings');
            }
            for (const row of data) {
                playerRatings.set(row.id, row);
                botData.set(row.id, row);
            }
        }

        if (humanPlayers.length > 0) {
            const { data, error } = await supabaseClient
                .from('user_elo_ratings')
                .select('*')
                .in('user_id', humanPlayers);
            if (error) {
                console.error('Error fetching ELO ratings:', error);
                throw new Error('Failed to fetch ELO ratings');
            }
            const existing = new Map((data ?? []).map((r: UserEloRating) => [r.user_id, r]));
            for (const id of humanPlayers) {
                // A first-time player has no row yet; start them at the base
                // rating — the batched upsert below creates the row.
                playerRatings.set(id, existing.get(id) ?? { elo_rating: 1000, games_played: 0 });
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
