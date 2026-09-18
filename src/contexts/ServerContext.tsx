import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import supabase from '../backend/Connector';
import { useParams } from 'next/navigation';
import { useAuth } from './AuthContext';
import { MAX_PLAYERS } from '@api/core/constants.ts';
import { ANIM_TIME_MS } from '@sdk/ts/gen/anim.bots.ts';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { animationFeed } from '../state/animationFeed';
import { holdPrivateChannel } from '../state/privateChannel';
import { cardKey, mergeHandOrder, reconcileHandMemory, displayedHand, mergeTableBattles } from '../state/clientReconcile';
import { keepPending, lobbyBoard, rearrangedBoard } from '../state/clientBoards';
import { ACTION_STATUS, REJECT_STALE_ROUND, decodeActionResponse, encodeAction, encodeActionRequest } from '@sdk/ts/wire/awire.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { GAME_STATUS, type TableView, type ViewCard } from '../state/view';
import { rejectMessage } from '../wasm/rejectMessages';
import { authoritativeVersion } from '../state/authoritativeVersion';
import { strings } from '../localization/strings';

type Card = ViewCard;

// An envelope (a player_views / spectator_views row, a create or meta response)
// read through the kernel's client slot: the board as this viewer sees it, or
// null when it does not read whole.
const readEnvelope = (bytes: Uint8Array): TableView | null => clientTable().adoptEnvelope(bytes);

// Decode the hex `view` blob stored in player_views. It is a BYTEA column, so a
// PostgREST select gives it as '\x'-prefixed hex and a Realtime postgres_changes
// payload as bare hex; the optional prefix is stripped either way. Tiny local
// helper so the dashboard read doesn't pull the replay codec into the main bundle.
const hexToBytes = (hex: string): Uint8Array => {
    const h = hex.startsWith('\\x') ? hex.slice(2) : hex;
    const out = new Uint8Array(h.length >> 1);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
    return out;
};

// How long a refused move waits for the pushes it was refused over before
// reconcileAfter loads the game: two flights, time for a late push.
//
// DERIVED FROM THE KERNEL'S FLIGHT, not from a TypeScript copy of it. This used
// to be `ANIMATION_TIME = 500` in src/constants/constants.ts, and every
// duration, gap and deadline in the product was that one number; the flight
// itself is the kernel's (c/src/anim_plan.h ANIM_TIME_MS, generated into
// sdk/ts/gen/anim.bots.ts), so the coupling cannot silently come apart.
const RECONCILE_GRACE_MS = 2 * ANIM_TIME_MS;

// Re-apply the local player's unconfirmed optimistic table cards onto an
// authoritatively-loaded board (reconnect resync), so a just-played card doesn't
// vanish then reappear. The kernel keeps them on the board (clientBoards.keepPending).
const applyOptimisticOverlay = (v: TableView): TableView => {
    const pending = optimisticOverlay.entries();
    return pending.length === 0 ? v : keepPending(v, pending) ?? v;
};

// Split contexts: actions are all useCallback([])-stable so this provider's
// value NEVER changes identity - components that only dispatch (buttons, drag
// handlers, feeds) subscribe via useServerActions() and stop re-rendering on
// every games/chat state change. State lives in its own context; useServer()
// merges both for backward compatibility (and re-renders on state changes,
// exactly as before the split).
const ServerActionsContext = createContext<ServerActionsType | null>(null);
const ServerStateContext = createContext<ServerStateType | null>(null);

// (The old `handsQuery` PostgREST projection is gone: the client no longer
// reads player_hands/games directly - it reads its own already-masked packed
// view straight from the player_views / spectator_views caches (a plain indexed
// RLS SELECT), with no edge round-trip. See docs/PLAYER_VIEWS.md.)

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const { user_id } = useAuth();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();
    // Every board this client holds, by game id: the kernel's TableView
    // snapshots (src/state/view.ts), as read or as changed optimistically.
    const [games, setGames] = useState<{ [key: string]: TableView }>({});

    // Update user names ref when games change: a seat's name by its player id,
    // from the table's identity.
    useEffect(() => {
        Object.values(games).forEach(view => {
            view.seats.forEach(seat => {
                userNamesRef.current[seat.id] = seat.name;
            });
        });
    }, [games]);

    // Chat messages state - keyed by game_id
    const [chatMessages, setChatMessages] = useState<{ [key: string]: any[] }>({});

    // Spectator mode state - tracks which games user is spectating
    const [spectatorGames, setSpectatorGames] = useState<Set<string>>(new Set());

    const [gameLoadError, setGameLoadError] = useState<string | null>(null);

    // A transient, localized notice shown when the server rejects a move because
    // a round closed before it landed (REJECT_STALE_ROUND, the stale-intent race
    // in docs/WEB_RACE_BUG_HANDOFF.md). The optimistic revert already happened
    // off the pickup broadcast; this just tells the user WHY their card came back.
    const [staleRoundNotice, setStaleRoundNotice] = useState<string | null>(null);
    const staleNoticeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
    const showStaleRoundNotice = useCallback(() => {
        const lang = (typeof localStorage !== 'undefined' && localStorage.getItem('foolish_language')) || 'en';
        const table = strings[lang] ?? strings.en;
        setStaleRoundNotice(table.staleRoundReject ?? strings.en.staleRoundReject);
        if (staleNoticeTimer.current) clearTimeout(staleNoticeTimer.current);
        staleNoticeTimer.current = setTimeout(() => setStaleRoundNotice(null), 4000);
    }, []);

    const [game_id, setGameId] = useState<string | null>(null);

    // The active game. The route param is the source of truth whenever it is
    // present - it is what AnimationContext, the broadcast version gate and
    // RealtimeAnimationFeed all key off - so actions must target it too, or a
    // move fired mid-navigation goes to the previously-selected game. The state
    // value only bridges flows that happen before navigation (create/join from
    // the dashboard).
    const active_game_id = url_game_id ?? game_id;

    // Live mirrors of state that the action callbacks read. Every action below
    // is useCallback([]) so the ServerActionsContext value NEVER changes
    // identity; reading through refs (synced after each commit) keeps those
    // frozen closures from ever serving a stale user/game/games value to a
    // handler that fires later.
    const gamesRef = useRef(games);
    const userIdRef = useRef(user_id);
    const activeGameIdRef = useRef<string | null>(null);
    const spectatorGamesRef = useRef(spectatorGames);
    // The game currently on screen (the ROUTE param), i.e. the one whose live
    // updates RealtimeAnimationFeed owns via the gu- animation stream. The
    // dashboard player_views subscription must NOT push snapshots into this game
    // or it would snap past the in-flight animation to the final state.
    const urlGameIdRef = useRef<string | undefined>(url_game_id);
    useEffect(() => { gamesRef.current = games; }, [games]);
    useEffect(() => { userIdRef.current = user_id; }, [user_id]);
    useEffect(() => { activeGameIdRef.current = active_game_id; }, [active_game_id]);
    useEffect(() => { spectatorGamesRef.current = spectatorGames; }, [spectatorGames]);
    useEffect(() => { urlGameIdRef.current = url_game_id; }, [url_game_id]);

    // Local hand order state - keyed by game_id
    // Thinking we just need one tbh
    const [localHandOrders, setLocalHandOrders] = useState<{ [key: string]: Card[] }>({});

    // Use ref to avoid closure issues in WebSocket handler
    const gameIdRef = useRef<string | null>(null);
    const userNamesRef = useRef<{ [userId: string]: string }>({});

    // Use ref to prevent duplicate user effect executions
    const prevUserRef = useRef<string | null>(null);

    // Track ongoing loadGame calls to prevent duplicates
    const loadGamePromises = useRef<Map<string, Promise<{ game_id: string }>>>(new Map());

    // Track ongoing getUserGames call to prevent duplicates
    const getUserGamesPromise = useRef<Promise<void> | null>(null);

    // The chat: channel held for a seated game (holdPrivateChannel), and how to
    // leave it: on teardown, and when the seat is given up.
    const chatHoldRef = useRef<{ gameId: string; stop: () => void } | null>(null);
    const stopChat = () => {
        chatHoldRef.current?.stop();
        chatHoldRef.current = null;
    };

    // The spectator's game-{id} stream, by game id: the channel object itself, never
    // a lookup by topic (realtime-js names a channel 'realtime:game-{id}', so a
    // lookup by the bare topic found nothing and a join left the stream behind).
    const spectatorChannels = useRef<Map<string, ReturnType<typeof supabase.channel>>>(new Map());
    const watchGame = (gameId: string) => {
        if (spectatorChannels.current.has(gameId)) return;
        const gameChannel = supabase.channel(`game-${gameId}`, { config: { private: true } });
        spectatorChannels.current.set(gameId, gameChannel);
        // Spectators get LIVE game updates too: the server broadcasts the
        // fully-masked (seat -1) animation stream to this game-<id> topic, built by
        // the same encoder the players' gu-<id>-<user> streams use. Republish it into
        // animationFeed exactly like RealtimeAnimationFeed does for players - the
        // packed envelope carries no JS state, so attach the game id so the consumer
        // can pick the decode roster.
        supabase.realtime.setAuth().then(() => {
            if (spectatorChannels.current.get(gameId) !== gameChannel) return;
            gameChannel
                .on('broadcast', { event: 'animation_events' }, (payload) => {
                    animationFeed.publish({ ...payload.payload, game_id: gameId });
                })
                .subscribe((status, err) => status === 'SUBSCRIBED'
                    ? console.log('Connected to game channel:', `game-${gameId}`)
                    : console.error('Game channel error:', err));
        });
    };
    const unwatchGame = (gameId: string) => {
        const gameChannel = spectatorChannels.current.get(gameId);
        if (!gameChannel) return;
        spectatorChannels.current.delete(gameId);
        supabase.removeChannel(gameChannel);
    };

    useEffect(() => {
        if (url_game_id) {
            gameIdRef.current = url_game_id;
            setGameId(url_game_id);
            setGameLoadError(null); // Clear any previous errors

            // Set local hand order if we already have the game data
            if ((games[url_game_id]?.mySeat ?? -1) >= 0) {
                setLocalHandOrders(prev => ({ ...prev, [url_game_id]: [...games[url_game_id].myHand] }));
            }

            // Only load if we don't have this game data yet
            if (!games[url_game_id]) {
                loadGame(url_game_id).catch(error => {
                    setGameLoadError(url_game_id); // Set error for this specific game
                });
            }
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [url_game_id]);



    // Keep ref in sync with state


    useEffect(() => {
        // Skip if user hasn't actually changed
        if (prevUserRef.current === user_id) {
            return;
        }

        prevUserRef.current = user_id;

        if (user_id) {
            // Only call getUserGames if we don't have a specific game loaded
            // If we have a URL game, loadGame will handle it
            if (!url_game_id) {
                getUserGames();
            }
        }

        // cleanup realtime subscriptions
        return () => {
            // Leave the chat: channel and cancel its pending rejoin, so it doesn't
            // re-subscribe to a game we're navigating away from.
            stopChat();
            // Remove subscriptions one channel at a time instead of removeAllChannels().
            // removeAllChannels() calls socket.disconnect() unconditionally, force-closing
            // the websocket (close code 1005) on every game switch - that 1005 then fans
            // out as a CHANNEL_ERROR to the channels being created for the next game.
            // Per-channel removeChannel() instead routes through realtime-js's deferred
            // disconnect (disconnectOnEmptyChannelsAfterMs), which is cancelled as soon as
            // the next game subscribes, so the socket is never bounced during a fast switch.
            // ONLY this context's spectator game-… channel (chat:… is left above): the
            // gu-… animation channel is owned and torn down by RealtimeAnimationFeed -
            // removing it here raced its own cleanup/reconnect handling. The pv-…
            // dashboard-cache channel is user-scoped (not game-scoped) and owned by
            // its own effect below, so it must survive game navigation too.
            supabase.getChannels().forEach((channel) => {
                if (channel.topic.includes('gu-') || channel.topic.includes('pv-') || channel.topic.includes('chat:')) return;
                supabase.removeChannel(channel);
            });
            spectatorChannels.current.clear();
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id, url_game_id]);


    // NOTE: there is deliberately no gu-<game>-<user> subscription here. That
    // personalized channel is owned by RealtimeAnimationFeed (the animation
    // pipeline); a second subscription from this context was pure duplicate
    // socket load - its only events were `private_message` (sender commented out
    // server-side) and `HAND_REARRANGED` (handler was a no-op).

    // A seated player's chat stream. chat:{game} admits only a member of the
    // game, so this is never asked for a spectator: a refused private join
    // stalls every channel on the shared socket (src/state/privateChannel.ts).
    const subscribeToChatMessages = async (gameId: string) => {
        if (chatHoldRef.current?.gameId === gameId) return;
        stopChat();
        const stop = holdPrivateChannel(`chat:${gameId}`, {
            bind: (channel) => {
                channel.on('broadcast', { event: 'INSERT' }, (payload) => {
                    handleChatMessage(payload.payload);
                });
            },
            onFailed: (status, err, retryInMs) => {
                console.error(`Chat channel ${status}, retrying in ${retryInMs}ms:`, err);
            },
        });
        chatHoldRef.current = { gameId, stop };
    };

    // mergeHandOrder / reconcileHandMemory / displayedHand / mergeTableBattles all
    // live in src/state/clientReconcile.ts (imported at the top) so they can be
    // unit-tested directly without React.

    // Helper method to update local hand order when game state changes. Sticky:
    // the arrangement memory keeps known slots and only grows with new cards, so a
    // card that's transiently absent (optimistically played then rejected) keeps
    // its slot instead of jumping to the end.
    const updateLocalHandOrder = (gameId: string, newHand: readonly Card[]) => {
        setLocalHandOrders(prev => ({ ...prev, [gameId]: reconcileHandMemory(prev[gameId] || [], newHand) }));
    };


    // Helper method to merge an incoming board with the one held for the game.
    // Every board names its viewer's seat and carries its version, so the
    // incoming board is taken whole except for the viewer's hand ORDER, which is
    // the client's own arrangement.
    const mergeGameData = (gameId: string, next: TableView, prevGames: { [key: string]: TableView }): TableView => {
        const prev = prevGames[gameId];
        let result: TableView = next;

        // Trust the incoming table (the merge policy lives in clientReconcile).
        if (prev) result = { ...result, battles: mergeTableBattles(prev.battles, next.battles) };

        // If we held this seat's hand before, preserve the hand order
        if (prev && prev.mySeat >= 0 && next.mySeat >= 0) {
            result = { ...result, myHand: mergeHandOrder(prev.myHand, next.myHand) };
        }

        // Update local hand order when game data changes
        if (result.mySeat >= 0) {
            updateLocalHandOrder(gameId, result.myHand);
        }

        return result;
    };

    // Dashboard live updates (docs/PLAYER_VIEWS.md): subscribe to THIS user's own
    // player_views rows (RLS-enforced) and push each committed masked snapshot
    // straight into `games`. This is the list-level counterpart of
    // RealtimeAnimationFeed's per-game animation stream - full snapshots instead
    // of event deltas, with no bespoke server fan-out. User-scoped (keyed on
    // user_id only), so it survives game navigation; the pv- channel is excluded
    // from the per-navigation channel teardown above. Best-effort: if the
    // subscription can't be established, the list still refreshes on the next
    // getUserGames / navigation.
    const playerViewsChannelRef = useRef<any>(null);
    useEffect(() => {
        if (!user_id) return;
        let cancelled = false;

        const applyRow = (row: any) => {
            if (!row?.view) return;
            try {
                const v = readEnvelope(hexToBytes(row.view));
                if (!v) return;
                // The on-screen game is animation-owned (RealtimeAnimationFeed):
                // pushing its final snapshot here would jump past the in-flight
                // animation. Let that pipeline apply the game being viewed; this
                // subscription keeps every OTHER game in the dashboard live.
                if (v.gameId === urlGameIdRef.current) return;
                setGames(prev => ({ ...prev, [v.gameId]: mergeGameData(v.gameId, v, prev) }));
            } catch { /* unreadable snapshot - ignore; the next fetch resyncs */ }
        };

        const pgChanges = { schema: 'public', table: 'player_views', filter: `player_id=eq.${user_id}` } as const;

        // setAuth() hands Realtime the caller's JWT so postgres_changes applies
        // player_views' RLS per row. NOT a `private` broadcast channel: for
        // postgres_changes the source table's RLS is the gate (the row filter
        // below is enforced server-side), not a realtime.messages topic policy -
        // marking it private would make the channel demand a 'pv-…' broadcast
        // policy that doesn't exist and fail to subscribe.
        supabase.realtime.setAuth().then(() => {
            if (cancelled) return;
            const channel = supabase.channel(`pv-${user_id}`);
            playerViewsChannelRef.current = channel;
            channel
                .on('postgres_changes', { event: 'INSERT', ...pgChanges }, (p: any) => applyRow(p.new))
                .on('postgres_changes', { event: 'UPDATE', ...pgChanges }, (p: any) => applyRow(p.new))
                .on('postgres_changes', { event: 'DELETE', ...pgChanges }, (p: any) => {
                    // The old row carries only the replica-identity (PK) columns -
                    // game_id + player_id - which is all we need to drop it.
                    const gid = p.old?.game_id;
                    if (!gid) return;
                    setGames(prev => {
                        if (!(gid in prev)) return prev;
                        const next = { ...prev };
                        delete next[gid];
                        return next;
                    });
                })
                .subscribe();
        }).catch(err => console.error('player_views subscription failed:', err));

        return () => {
            cancelled = true;
            if (playerViewsChannelRef.current) {
                const ch = playerViewsChannelRef.current;
                playerViewsChannelRef.current = null;
                supabase.removeChannel(ch).catch(() => { /* already closed */ });
            }
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id]);

    const handleChatMessage = (message: any) => {
        // Handle database changes for chat messages
        const { record: newRecord, old_record: oldRecord, table, operation } = message;

        if (table !== 'chat_messages') {
            return;
        }

        const gameId = newRecord?.game_id || oldRecord?.game_id;

        if (!gameId) {
            return;
        }

        if (operation === 'INSERT') {
            // Add new message with user info
            const messageWithUserInfo = {
                ...newRecord,
                sender_name: userNamesRef.current[newRecord.user_id] || 'Unknown'
            };

            setChatMessages(prev => {
                const existingMessages = prev[gameId] || [];
                // Check if message already exists to avoid duplicates
                const messageExists = existingMessages.some(msg => msg.id === newRecord.id);
                if (!messageExists) {
                    const newState = {
                        ...prev,
                        [gameId]: [...existingMessages, messageWithUserInfo]
                    };
                    return newState;
                }
                return prev;
            });
        }
    };

    const createGame = useCallback(async (): Promise<{ game_id: string }> => {
        // create now returns the caller's PACKED view buffer (like get_game) -
        // decode it with the shared codec, the same path loadGame uses. There is
        // no JSON body to fall back to any more - the server has one answer. The
        // server persists the game to the DB in the background AFTER responding,
        // so this returns as soon as the lobby is built (no create_game
        // round-trip on the critical path).
        const { data, error } = await supabase.functions.invoke('create', { body: {} });
        if (error) throw error;

        const bytes = typeof Blob !== 'undefined' && data instanceof Blob
            ? new Uint8Array(await data.arrayBuffer())
            : data instanceof ArrayBuffer ? new Uint8Array(data) : null;
        const view = bytes ? readEnvelope(bytes) : null;
        if (!view) throw new Error('create: unreadable packed response');

        setGameId(view.gameId);
        setGames(prev => ({ ...prev, [view.gameId]: mergeGameData(view.gameId, view, prev) }));
        // Subscribe to the new game's chat (the gu- animation channel is owned by
        // RealtimeAnimationFeed).
        subscribeToChatMessages(view.gameId).catch(console.error);
        return { game_id: view.gameId };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const joinGame = useCallback((gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', {
            type: 'join',
            game_id: gameId,
        }, {
            onSuccess: (view) => {
                setGameId(view.gameId);
                // The response is the joiner's own seated view. Apply it: the
                // gu- animation stream is joined only once client state shows
                // the seat (RealtimeAnimationFeed), and the join's own
                // broadcast went out before this user could receive it.
                setGames(prev => ({ ...prev, [view.gameId]: mergeGameData(view.gameId, view, prev) }));
                // Remove from spectator mode when joining
                setSpectatorGames(prev => {
                    const newSet = new Set(prev);
                    newSet.delete(gameId);
                    return newSet;
                });

                // Leave the spectator stream: the seat's own gu- stream
                // (RealtimeAnimationFeed) carries the game from here.
                unwatchGame(gameId);

                // Subscribe to the game's chat (the gu- animation channel is
                // owned by RealtimeAnimationFeed)
                subscribeToChatMessages(view.gameId).catch(console.error);
                loadChatHistory(view.gameId).catch(console.error);
            }
        })
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // start / add-bot / exit / continue are one consolidated `meta` endpoint
    // (dispatched on `type`) - fewer functions, faster deploys.
    const startGame = useCallback((gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', {
            type: 'start',
            game_id: gameId,
        })
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const addBot = useCallback((gameId: string, botId?: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', {
            type: 'add-bot',
            game_id: gameId,
            bot_id: botId,
        })
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const exitGame = useCallback((gameId: string, botId?: string, playerId?: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', {
            type: 'exit',
            game_id: gameId,
            bot_id: botId,
            player_id: playerId
        }, {
            onSuccess: (view) => {
                // If I gave up my own seat (not a bot's, not another player's),
                // I am watching now. The gu- stream is RealtimeAnimationFeed's to
                // leave once the board shows no seat; the chat: membership is gone.
                if (!botId && (!playerId || playerId === userIdRef.current)) {
                    setSpectatorGames(prev => new Set(prev).add(gameId));
                    // The answer is my view without the seat: apply it, as a join
                    // applies its own, so the page and the feed see the seat gone now.
                    setGames(prev => ({ ...prev, [view.gameId]: mergeGameData(view.gameId, view, prev) }));
                    stopChat();
                    watchGame(gameId);
                }
            }
        })
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // Hmm loading the url should add the player to the game.

    const loadGame = useCallback(async (gameId: string): Promise<{ game_id: string }> => {
        // Check if we already have an ongoing request for this game
        const existingPromise = loadGamePromises.current.get(gameId);
        if (existingPromise) {
            return existingPromise;
        }

        // Create new promise and cache it
        const gamePromise = loadGameInternal(gameId);
        loadGamePromises.current.set(gameId, gamePromise);

        // Clean up cache when promise resolves or rejects
        gamePromise.finally(() => {
            loadGamePromises.current.delete(gameId);
        });

        return gamePromise;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // Fast path for the game screen (docs/PLAYER_VIEWS.md): a PLAYER reads their
    // own already-masked view straight from player_views - a plain indexed RLS
    // SELECT, no edge round-trip. RLS scopes it to the caller, and the
    // (game_id, player_id) PK means at most one row. Returns null for a spectator
    // (no row), a cache miss (game predating the cache), or any failure - the
    // caller then falls back to spectator_views (the shared masked view).
    const loadGameFromCache = async (gameId: string): Promise<TableView | null> => {
        try {
            if (!userIdRef.current) return null;
            const { data, error } = await supabase
                .from('player_views')
                .select('view')
                .eq('game_id', gameId)
                .maybeSingle();
            if (error || !(data as any)?.view) return null;
            return readEnvelope(hexToBytes((data as any).view));
        } catch {
            return null;
        }
    };

    // Spectator fast path (docs/PLAYER_VIEWS.md): a NON-participant reads the
    // shared, fully-masked (seat -1) view straight from spectator_views - a plain
    // indexed RLS SELECT, no edge round-trip, replacing the get_game spectate
    // path. Readable by any authenticated user (the row carries no hidden state),
    // one row per game. Returns null on a miss/failure; the board's seat is
    // always -1 (a spectator has none). Live updates arrive over the game-<id> broadcast.
    const loadSpectatorFromCache = async (gameId: string): Promise<TableView | null> => {
        try {
            const { data, error } = await supabase
                .from('spectator_views')
                .select('view')
                .eq('game_id', gameId)
                .maybeSingle();
            if (error || !(data as any)?.view) return null;
            return readEnvelope(hexToBytes((data as any).view));
        } catch {
            return null;
        }
    };

    const loadGameInternal = async (gameId: string): Promise<{ game_id: string }> => {
        try {
            // Player fast path: the caller's own masked view from the
            // player_views cache. Falls back to the shared spectator_views row
            // (fully masked, no seat) for a non-participant - both are plain
            // indexed RLS SELECTs, no edge function.
            let view: TableView | null = await loadGameFromCache(gameId);

            if (!view) {
                view = await loadSpectatorFromCache(gameId);
            }

            // Neither cache had a row (game predates the caches, or was pruned):
            // there is no edge fallback anymore, so this is a genuine not-found.
            if (!view) throw new Error(`Game ${gameId} not found`);

            // Re-apply the local player's unconfirmed optimistic cards onto
            // the authoritative state so a reconnect resync doesn't make a
            // just-played card vanish-then-reappear (Q7). No-op on a normal
            // load (nothing optimistic pending) and for a spectator.
            const loaded = applyOptimisticOverlay(view);

            setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, loaded, prev) }));
            joinOrSubscribe(loaded);

            // Trigger the bot loop only if the caller is a player in a game with
            // AI players. Fire and forget - don't block UI rendering.
            if (loaded.mySeat >= 0 && loaded.seats.some(seat => seat.isAi)) {
                supabase.functions.invoke('action', { body: { game_id: gameId, type: 'bump' } }).catch(() => { });
            }

            return { game_id: gameId };

        } catch (error) {
            throw error;
        }
    };

    const joinOrSubscribe = (view: TableView) => {
        const gameId = view.gameId;

        // Set game_id state and game data first, then load chat history
        setGameId(gameId);
        //setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, game, prev) }));

        // Load chat history with game data
        loadChatHistory(gameId).catch(console.error);

        if (view.mySeat >= 0) {
            // Player is in the game - remove from spectator mode if present
            setSpectatorGames(prev => {
                const newSet = new Set(prev);
                newSet.delete(gameId);
                return newSet;
            });
            // gu- (animation) subscription is handled by RealtimeAnimationFeed
            subscribeToChatMessages(gameId).catch(console.error);
            return;
        }

        // Check if user is intentionally spectating this game
        const isSpectating = spectatorGamesRef.current.has(gameId);

        // no game self + waiting + not spectating + room available -> join
        // no game self + (not waiting OR spectating OR no room) -> subscribe to game
        if (!isSpectating && view.status === GAME_STATUS.WAITING && view.seats.length < MAX_PLAYERS) {
            // Auto-join only if not intentionally spectating
            joinGame(gameId).catch(console.error);
        } else {
            // Subscribe as spectator. No chat: for a spectator: its policy admits
            // members only, and a refused join stalls the game- stream on the same socket.
            watchGame(gameId);
        }
    }

    const loadChatHistory = async (gameId: string): Promise<void> => {
        try {
            const { data, error } = await supabase
                .from('chat_messages')
                .select('*')
                .eq('game_id', gameId)
                .order('created_at', { ascending: true })
                .limit(100); // Load last 100 messages

            if (error) {
                console.error('Error loading chat history:', error);
                return;
            }

            // Transform messages to include sender names from userNamesRef
            const messagesWithNames = data.map(msg => ({
                ...msg,
                sender_name: userNamesRef.current[msg.user_id] || 'Unknown'
            }));

            setChatMessages(prev => ({
                ...prev,
                [gameId]: messagesWithNames
            }));
        } catch (error) {
            console.error('Error in loadChatHistory:', error);
        }
    };

    // A move goes to the server FIRST - the server is authoritative and rejects an
    // illegal move, so the round-trip isn't gated on local validation. The body is
    // the packed awire buffer: the caller-supplied bytes (already validated against
    // the kernel) or a fresh encode for direct callers.
    //
    // THE BOARD THE MOVE LEAVES IS NOT THIS FILE'S ANY MORE. It used to land here
    // on a SECOND ANIMATION_TIME timer, in a different file from the flight it was
    // meant to follow and coupled to it by nothing but the two reading the same
    // constant: if the queue was busy the board arrived while some other card was
    // still in the air. The prediction is one of the kernel's steps now, and its
    // board commits when that step lands (AnimationContext, ClientAnimationEvent
    // commit_board / commit_if) - one clock, one plan, one landing.
    const playMove = (move: Uint8Array): Promise<{ game_id: string }> =>
        invokePackedAction(activeGameIdRef.current!, move);

    const attack = useCallback((cards: Card[], wire?: Uint8Array): Promise<{ game_id: string }> => {
        return playMove(wire ?? encodeAction({ kind: 'attack', cards }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const pass = useCallback((cards: Card[], wire?: Uint8Array): Promise<{ game_id: string }> => {
        return playMove(wire ?? encodeAction({ kind: 'pass', cards }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const pickup = useCallback((wire?: Uint8Array): Promise<{ game_id: string }> => {
        return playMove(wire ?? encodeAction({ kind: 'pickup' }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const cover = useCallback((coverCards: Card[], attackCards: Card[], wire?: Uint8Array): Promise<{ game_id: string }> => {
        return playMove(wire ?? encodeAction({ kind: 'cover', cards: coverCards, attack_cards: attackCards }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const good = useCallback((): Promise<{ game_id: string }> => {
        // Packed like the other moves; `good` has no optimistic patch. MOOT
        // (the game ended under us) resolves as success, same as before.
        return invokePackedAction(activeGameIdRef.current!, encodeAction({ kind: 'good' }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const sendMessage = useCallback(async (message: string): Promise<void> => {
        try {
            const gid = activeGameIdRef.current;
            const user_id = userIdRef.current;
            if (!gid || !user_id) {
                const error = new Error('No game or user available');
                throw error;
            }

            if (!message || message.trim() === '') {
                const error = new Error('Message cannot be empty');
                throw error;
            }

            if (message.length > 1000) {
                const error = new Error('Message is too long');
                throw error;
            }

            const trimmedMessage = message.trim();

            // Save message to database - the trigger will handle broadcasting
            const { error } = await supabase
                .from('chat_messages')
                .insert({
                    game_id: gid,
                    user_id: user_id,
                    message: trimmedMessage,
                    is_system: false
                });

            if (error) {
                throw error;
            }

        } catch (error) {
            throw error;
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const updateGameName = useCallback((gameId: string, name: string): Promise<{ game_id: string }> => {
        const previousName = gamesRef.current[gameId]?.title;

        setGames(prev => ({
            ...prev,
            [gameId]: {
                ...prev[gameId],
                title: name
            }
        }));

        const revert = () => {
            if (!previousName) {
                return;
            }
            setGames(prev => ({
                ...prev,
                [gameId]: {
                    ...prev[gameId],
                    title: previousName
                }
            }));
        }

        return invokeGameFunctions('meta', {
            type: 'update-name',
            game_id: gameId,
            new_name: name
        }, {
            onError: revert
        });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const rearrangePlayer = useCallback((gameId: string, playerIds: string[]): Promise<{ game_id: string }> => {
        // A lobby's seats, reordered by their player ids (a lobby holds no cards,
        // so the seats are all a reorder moves).
        const previousSeats = gamesRef.current[gameId]?.seats ? [...gamesRef.current[gameId].seats] : [];
        if (previousSeats.length === 0) {
            return Promise.reject(new Error(`Cannot rearrange players`));
        }
        const rearrangedSeats = playerIds.map(playerId =>
            previousSeats.find(s => s.id === playerId)!
        );
        setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], seats: rearrangedSeats } }));

        const revert = () => {
            if (previousSeats.length === 0) {
                return;
            }
            setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], seats: previousSeats } }));
        }

        return invokeGameFunctions('meta', {
            type: 'rearrange-players',
            game_id: gameId,
            new_order: playerIds
        }, {
            onError: revert
        });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const rearrangeHand = useCallback((gameId: string, cardIndices: number[]): Promise<{ game_id: string }> => {
        // The reorder is debounced (DragContext.scheduleCardRearrangeUpdate), so by
        // the time it flushes the hand may have changed (a card played, drawn, or
        // picked up). Indices computed against the OLD hand can now be out-of-range
        // or non-bijective; applying them optimistically would mint an `undefined`
        // slot into the hand, and the next render crashes reading `card.suit`. The
        // kernel orders the hand only by a true permutation (game_rearrange_hand, the
        // server's own check); a stale reorder is abandoned rather than materialized.
        const board = gamesRef.current[gameId];
        const arranged = board ? rearrangedBoard(board, cardIndices) : { kind: 'no-hand' as const };
        if (arranged.kind === 'no-hand') {
            return Promise.reject(new Error(`Cannot rearrange hand`));
        }
        if (arranged.kind === 'not-an-order') {
            return Promise.resolve({ game_id: gameId });
        }

        const previousHand = board!.myHand;
        const rearrangedHand = arranged.view.myHand;
        setGames(prev => ({
            ...prev, [gameId]: {
                ...prev[gameId],
                myHand: rearrangedHand
            }
        }));

        const revert = () => {
            setGames(prev => ({
                ...prev, [gameId]: {
                    ...prev[gameId],
                    myHand: previousHand
                }
            }));
        }

        return invokeGameFunctions('meta', {
            type: 'rearrange-hand',
            game_id: gameId,
            card_indices: cardIndices
        }, {
            onError: revert
        });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const getUserGames = useCallback(async (): Promise<void> => {
        // Check if we already have an ongoing getUserGames request
        if (getUserGamesPromise.current) {
            return getUserGamesPromise.current;
        }

        // Create new promise and cache it
        const gamesPromise = getUserGamesInternal();
        getUserGamesPromise.current = gamesPromise;

        // Clean up cache when promise resolves or rejects
        gamesPromise.finally(() => {
            getUserGamesPromise.current = null;
        });

        return gamesPromise;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const getUserGamesInternal = async (): Promise<void> => {
        const user_id = userIdRef.current;
        if (!user_id) {
            return;
        }

        // The dashboard list is a plain indexed RLS SELECT straight from the
        // player_views cache (docs/PLAYER_VIEWS.md) - no edge function, no cold
        // start, no per-viewer masking on read (rows are masked at write time).
        // Each row's `view` is the caller's packed single-game envelope, read
        // here through the kernel's client slot (readEnvelope). player_views is
        // kept complete by commit_game / create_game, so there is no fallback: an
        // empty result simply means the user has no games.
        try {
            const { data: rows, error } = await supabase
                .from('player_views')
                .select('view, status, version')
                .eq('player_id', user_id)
                .order('updated_at', { ascending: false });
            if (error) {
                console.error('Error fetching user games from player_views:', error);
                return;
            }
            const games: { [key: string]: TableView } = {};
            for (const row of rows ?? []) {
                try {
                    const v = readEnvelope(hexToBytes((row as any).view));
                    if (!v) continue;
                    games[v.gameId] = v;
                } catch { /* skip an unreadable row */ }
            }
            setGames(prev => ({ ...prev, ...games }));
        } catch (e) {
            console.error('Error in getUserGames:', e);
        }
    };

    const continueGame = useCallback((gameId: string): Promise<{ game_id: string }> => {
        // Optimistic transition: reset the finished game to its lobby state LOCALLY
        // right now, so the win screen swaps to the lobby instantly instead of
        // waiting out the meta round-trip (the WinScreen→Lobby swap is purely
        // status-driven - WinScreen renders null once status !== GAME_OVER). The
        // server `continue` runs in the background; its authoritative reset (the
        // MAGIC_TRANSITION broadcast + response) reconciles with this - the kernel
        // makes both (game_reset_to_lobby), so there's no visible snap. On failure we
        // roll back to the finished game so the user can retry.
        const prev = gamesRef.current[gameId];
        const optimistic = prev && prev.status === GAME_STATUS.GAME_OVER ? lobbyBoard(prev) : null;
        if (prev && optimistic) {
            setGames(cur => ({ ...cur, [gameId]: optimistic }));

            invokeGameFunctions('meta', { type: 'continue', game_id: gameId }).catch(err => {
                console.error('continue failed - rolling back to the finished game:', err);
                setGames(cur => (cur[gameId] === optimistic ? { ...cur, [gameId]: prev } : cur));
            });
            return Promise.resolve({ game_id: gameId });
        }

        // Not a finished game we hold locally: no optimistic state to build, just
        // await the server (the authoritative update arrives via broadcast).
        return invokeGameFunctions('meta', { type: 'continue', game_id: gameId });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // The lobby / meta transport. The REQUEST is still JSON ({type, game_id, ...}
    // is a command, not game state), but the RESPONSE is the packed view
    // envelope - the same bytes `create` returns, player_views stores and the
    // realtime feed pushes - so no game state crosses this wire as JSON.
    // functions-js hands an octet-stream response back as a Blob.
    const invokeGameFunctions = async (
        functionName: string,
        body: any = {},
        options: {
            onSuccess?: (view: TableView) => void;
            onError?: (error: any) => void;
        } = {}
    ): Promise<{ game_id: string }> => {
        try {
            const { data } = await supabase.functions.invoke(functionName, { body });

            let bytes: Uint8Array | null = null;
            if (typeof Blob !== 'undefined' && data instanceof Blob) {
                bytes = new Uint8Array(await data.arrayBuffer());
            } else if (data instanceof ArrayBuffer) {
                bytes = new Uint8Array(data);
            }
            const view = bytes ? readEnvelope(bytes) : null;

            if (!view || !view.gameId) {
                throw new Error(`Invalid response from ${functionName}: missing game ID`);
            }

            const game_id = view.gameId;

            options.onSuccess?.(view);

            return { game_id };

        } catch (error) {
            options.onError?.(error);
            throw error;
        }
    }

    // The packed move transport (docs/PACKED_WIRE_CUTOVER.md): POST the awire
    // bytes - the exact buffer the kernel validated - wrapped in the binary
    // request envelope. functions-js only passes a body through with
    // Content-Type: application/octet-stream when it is a Blob or an
    // ArrayBuffer (a Uint8Array would be JSON.stringified - see
    // @supabase/functions-js FunctionsClient.invoke), so the envelope rides in
    // a Blob; octet-stream responses come back as a Blob too.
    // 'bump' and all meta ops stay JSON via invokeGameFunctions.
    // After a refused (or moot) move whose answer names a newer version than the
    // page has applied, the page is behind the server: the pushes that would have
    // told it are late or lost. Give a late push RECONCILE_GRACE_MS to arrive and
    // animate as usual; if the page is still behind then, load the game, so a lost
    // push leaves the board stale for about a second instead of until the next move.
    const reconcileTimers = useRef<Map<string, ReturnType<typeof setTimeout>>>(new Map());
    const reconcileAfter = (gameId: string, serverVersion: number) => {
        const seen = authoritativeVersion(gameId);
        if (seen !== undefined && serverVersion <= seen) return;
        const timers = reconcileTimers.current;
        const prior = timers.get(gameId);
        if (prior) clearTimeout(prior);
        timers.set(gameId, setTimeout(() => {
            timers.delete(gameId);
            const now = authoritativeVersion(gameId);
            if (now !== undefined && now >= serverVersion) return;
            loadGame(gameId).catch(() => { /* the next push or resubscribe resync catches up */ });
        }, RECONCILE_GRACE_MS));
    };

    const invokePackedAction = async (gameId: string, wire: Uint8Array): Promise<{ game_id: string }> => {
        // Stamp the move with the version the client composed it against, so the
        // server's round-boundary guard can reject it if a round closed in the
        // meantime (docs/WEB_RACE_BUG_HANDOFF.md). undefined => the legacy v1
        // envelope, unguarded (we've not yet seen an authoritative version).
        const req = encodeActionRequest(gameId, wire, authoritativeVersion(gameId));
        // Cast: encodeActionRequest builds a fresh, non-shared buffer; TS just
        // types Uint8Array over ArrayBufferLike, which BlobPart rejects.
        const { data, error } = await supabase.functions.invoke('action', { body: new Blob([req as Uint8Array<ArrayBuffer>]) });
        if (error) {
            throw error;
        }
        let bytes: Uint8Array | null = null;
        if (typeof Blob !== 'undefined' && data instanceof Blob) {
            bytes = new Uint8Array(await data.arrayBuffer());
        } else if (data instanceof ArrayBuffer) {
            bytes = new Uint8Array(data);
        }
        const resp = bytes ? decodeActionResponse(bytes) : null;
        if (!resp) {
            throw new Error('Invalid response from action: unreadable packed response');
        }
        // A move that did not apply was judged against a game newer than the one
        // on screen: catch up with it (reconcileAfter), whether or not its pushes
        // ever arrive.
        if (resp.status !== ACTION_STATUS.APPLIED) reconcileAfter(gameId, resp.version);
        if (resp.status === ACTION_STATUS.REJECTED) {
            // A stale-round reject is the one rejection the user should SEE: the
            // move was kernel-legal, just aimed at a round that closed first, so
            // surface the localized notice (the revert already fired off the
            // pickup broadcast). Every other reject stays console-only diagnostics
            // - callers revert the optimistic state either way.
            if (resp.rejectCode === REJECT_STALE_ROUND) showStaleRoundNotice();
            throw new Error(rejectMessage(resp.rejectCode));
        }
        // APPLIED - or MOOT (the move lost the end-game race, a no-op): both
        // resolve as success, mirroring the old JSON path's data.id check.
        return { game_id: gameId };
    };

    const updateGameState = useCallback((gameId: string, view: TableView) => {
        // The animation queue commits each sequence's boards as its flights land,
        // and a push whose events were all mine commits at once. So an older push
        // still playing out can land after a newer one: its board never replaces
        // the newer version the store already holds.
        setGames(prev => (prev[gameId] && view.version < prev[gameId].version
            ? prev
            : { ...prev, [gameId]: mergeGameData(gameId, view, prev) }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const setLocalHandOrder = useCallback((order: readonly Card[]) => {
        const gid = activeGameIdRef.current;
        if (gid) {
            // Sticky: take the dragged order of the visible cards, then keep
            // any remembered (currently-absent) cards so their slots survive.
            setLocalHandOrders(prev => {
                const inOrder = new Set(order.map(cardKey));
                const remembered = (prev[gid] || []).filter(c => !inOrder.has(cardKey(c)));
                return { ...prev, [gid]: [...order, ...remembered] };
            });
        }
    }, []);

    // Every entry is a stable useCallback, so this object is created once and
    // the actions context never re-renders its consumers.
    const actions: ServerActionsType = useMemo(() => ({
        createGame, joinGame, startGame, addBot, exitGame,
        attack, pass, pickup, cover, good,
        sendMessage, getUserGames, updateGameState, updateGameName,
        rearrangePlayer, rearrangeHand, continueGame, loadGame, setLocalHandOrder,
    }), [createGame, joinGame, startGame, addBot, exitGame, attack, pass, pickup, cover, good,
        sendMessage, getUserGames, updateGameState, updateGameName, rearrangePlayer, rearrangeHand,
        continueGame, loadGame, setLocalHandOrder]);

    const state: ServerStateType = useMemo(() => ({
        game_id: active_game_id,
        view: games[active_game_id!] ?? null,
        views: games,
        gameLoadError,
        staleRoundNotice,
        chatMessages: chatMessages[active_game_id!] || [],
        // The rendered hand: the board's own hand, deduped and ordered by the
        // sticky arrangement memory. Guarantees no duplicates and no on-table
        // cards in the hand, and keeps a rejected card in its original slot.
        localHandOrder: displayedHand(localHandOrders[active_game_id!] || [], games[active_game_id!]?.myHand || []),
    }), [games, active_game_id, gameLoadError, staleRoundNotice, chatMessages, localHandOrders]);

    return (
        <ServerActionsContext.Provider value={actions}>
            <ServerStateContext.Provider value={state}>
                {children}
            </ServerStateContext.Provider>
        </ServerActionsContext.Provider>
    );
};

interface ServerActionsType {
    createGame: () => Promise<{ game_id: string }>;
    joinGame: (gameId: string) => Promise<{ game_id: string }>;
    startGame: (gameId: string) => Promise<{ game_id: string }>;
    addBot: (gameId: string, botId?: string) => Promise<{ game_id: string }>;
    exitGame: (gameId: string, botId?: string, playerId?: string) => Promise<{ game_id: string }>;
    // A move goes to the server and nothing else happens here: the board it
    // leaves rides its own flight in the animation pipeline, so there is no
    // second timer and no validity thunk to gate one.
    // The optional `wire` is the move's awire buffer (encodeAction) - passed by
    // callers that already validated those bytes so the POST body is bit-identical;
    // encoded on the spot when absent.
    attack: (cards: Card[], wire?: Uint8Array) => Promise<{ game_id: string }>;
    pass: (cards: Card[], wire?: Uint8Array) => Promise<{ game_id: string }>;
    pickup: (wire?: Uint8Array) => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[], wire?: Uint8Array) => Promise<{ game_id: string }>;
    good: () => Promise<{ game_id: string }>;
    sendMessage: (message: string) => Promise<void>;
    getUserGames: () => Promise<void>;
    updateGameState: (gameId: string, view: TableView) => void;
    updateGameName: (gameId: string, name: string) => Promise<{ game_id: string }>;
    rearrangePlayer: (gameId: string, playerIds: string[]) => Promise<{ game_id: string }>;
    rearrangeHand: (gameId: string, cardIndices: number[]) => Promise<{ game_id: string }>;
    continueGame: (gameId: string) => Promise<{ game_id: string }>;
    /** Refetch authoritative game state over REST. Used to resync after a
     *  realtime reconnect, where broadcasts missed during the gap are lost. */
    loadGame: (gameId: string) => Promise<{ game_id: string }>;
    setLocalHandOrder: (order: readonly Card[]) => void;
}

interface ServerStateType {
    game_id: string | null;
    /** The board on screen: the kernel's TableView snapshot (src/state/view.ts). */
    view: TableView | null;
    /** Every board this client holds, by game id. */
    views: { [key: string]: TableView };
    gameLoadError: string | null;
    /** A localized notice when the server rejected a move as stale-round (a round
     *  closed before it landed); null when there is nothing to show. Auto-clears. */
    staleRoundNotice: string | null;
    chatMessages: any[];
    localHandOrder: readonly Card[];
}

type ServerContextType = ServerActionsType & ServerStateType;

/** Actions only - the value is referentially stable for the provider's whole
 *  lifetime, so consumers that only dispatch never re-render on state churn. */
export const useServerActions = (): ServerActionsType => {
    const context = useContext(ServerActionsContext);
    if (!context) {
        throw new Error('useServerActions must be used within a ServerProvider');
    }
    return context;
};

export const useServer = (): ServerContextType => {
    const actions = useContext(ServerActionsContext);
    const state = useContext(ServerStateContext);
    if (!actions || !state) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
    return useMemo(() => ({ ...actions, ...state }), [actions, state]);
};

// Provider for the replay screen: holds a local games map and serves it
// through the same context the live display components AND AnimationProvider
// read. updateGameState really updates (plain replacement - no optimistic
// merging here), which is what lets the real animation pipeline drive the
// replay: each synthesized event commits its game_state snapshot exactly
// like a live broadcast would. Every server method is inert.
export const ReplayServerProvider = ({ gameId, initialGame, children }: {
    gameId: string,
    initialGame: TableView,
    children: React.ReactNode,
}) => {
    const [games, setGames] = useState<{ [key: string]: TableView }>({ [gameId]: initialGame });

    const updateGameState = useCallback((gid: string, view: TableView) => {
        setGames(prev => ({ ...prev, [gid]: view }));
    }, []);

    // The tutorial seats the learner, so the live ActionButtons + drag system
    // need a hand to render. Mirror the board's own hand (a plain replay's
    // viewer is a spectator, so this stays []). Reordering is a no-op here - the
    // tutorial doesn't need drag-to-rearrange, only drag-to-play.
    const localHandOrder = (games[gameId] ?? initialGame).myHand;

    const actions: ServerActionsType = useMemo(() => {
        const noop = async () => ({ game_id: gameId });
        return {
            createGame: noop,
            joinGame: noop,
            startGame: noop,
            addBot: noop,
            exitGame: noop,
            attack: noop,
            pass: noop,
            pickup: noop,
            cover: noop,
            good: noop,
            sendMessage: async () => { },
            getUserGames: async () => { },
            updateGameState,
            updateGameName: noop,
            rearrangePlayer: noop,
            rearrangeHand: noop,
            continueGame: noop,
            loadGame: noop,
            setLocalHandOrder: () => { },
        };
    }, [gameId, updateGameState]);

    const state: ServerStateType = useMemo(() => ({
        game_id: gameId,
        view: games[gameId] ?? initialGame,
        views: games,
        gameLoadError: null,
        staleRoundNotice: null,
        chatMessages: [],
        localHandOrder,
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }), [games, gameId, initialGame, localHandOrder]);

    return (
        <ServerActionsContext.Provider value={actions}>
            <ServerStateContext.Provider value={state}>
                {children}
            </ServerStateContext.Provider>
        </ServerActionsContext.Provider>
    );
};
