import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { Card, PersonalGame, PublicGame, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '@shared/types.ts';
import supabase from '../backend/Connector';
import { useParams } from 'next/navigation';
import { useAuth } from './AuthContext';
import { MAX_PLAYERS } from '@shared/constants.ts';
import { get_next_player_index, card_comp } from '@shared/common_utils.ts';
import { ANIMATION_TIME } from '../constants/constants';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { cardKey, mergeHandOrder, reconcileHandMemory, displayedHand, mergeTableBattles, applyOverlayEntries } from '../state/clientReconcile';
import { ACTION_STATUS, decodeActionResponse, encodeAction, encodeActionRequest } from '@shared/wire/awire.ts';
import { decodePackedGame } from '@shared/wire/view.ts';
import { rejectMessage } from '../wasm/rejectMessages';

// Decode the bare-hex (no \x prefix) `view` blob stored in player_views. Tiny
// local helper so the dashboard read doesn't pull the replay codec into the
// main bundle.
const hexToBytes = (hex: string): Uint8Array => {
    const h = hex.startsWith('\\x') ? hex.slice(2) : hex;
    const out = new Uint8Array(h.length >> 1);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
    return out;
};

// Re-apply the local player's unconfirmed optimistic table cards onto an
// authoritatively-loaded game (reconnect resync), so a just-played card doesn't
// vanish then reappear. Thin wrapper over the shared, unit-tested applyOverlayEntries.
const applyOptimisticOverlay = (g: PersonalGame): void => {
    applyOverlayEntries(g, optimisticOverlay.entries());
};

// Split contexts: actions are all useCallback([])-stable so this provider's
// value NEVER changes identity — components that only dispatch (buttons, drag
// handlers, feeds) subscribe via useServerActions() and stop re-rendering on
// every games/chat state change. State lives in its own context; useServer()
// merges both for backward compatibility (and re-renders on state changes,
// exactly as before the split).
const ServerActionsContext = createContext<ServerActionsType | null>(null);
const ServerStateContext = createContext<ServerStateType | null>(null);

// (The old `handsQuery` PostgREST projection is gone: the client no longer
// reads player_hands/games directly — game state is fetched, already
// personalized from the packed kernel blob, via the get_game / get_my_games
// edge functions. See docs/STATE_BLOB_CUTOVER.md.)

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const { user_id } = useAuth();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();
    // keep a state of games
    // maybe ref idk
    const [games, setGames] = useState<{ [key: string]: (PersonalGame) }>({});

    // Update user names ref when games change
    useEffect(() => {
        Object.values(games).forEach(game => {
            if (game.players) {
                game.players.forEach(player => {
                    userNamesRef.current[player.player_id] = player.name;
                });
            }
        });
    }, [games]);

    // Chat messages state - keyed by game_id
    const [chatMessages, setChatMessages] = useState<{ [key: string]: any[] }>({});

    // Spectator mode state - tracks which games user is spectating
    const [spectatorGames, setSpectatorGames] = useState<Set<string>>(new Set());

    const [gameLoadError, setGameLoadError] = useState<string | null>(null);

    const [game_id, setGameId] = useState<string | null>(null);

    // The active game. The route param is the source of truth whenever it is
    // present — it is what AnimationContext, the broadcast version gate and
    // RealtimeAnimationFeed all key off — so actions must target it too, or a
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

    // Simple reconnection state
    const chatChannelRetryInterval = useRef(500); // Start with 0.5 seconds
    // Handle for the pending reconnect timer so we can cancel it on teardown
    // and avoid leaked retries re-subscribing to games the user has left.
    const chatChannelRetryTimeout = useRef<ReturnType<typeof setTimeout> | null>(null);
    const MAX_RETRY_INTERVAL = 5000; // Cap at 5 seconds

    useEffect(() => {
        if (url_game_id) {
            gameIdRef.current = url_game_id;
            setGameId(url_game_id);
            setGameLoadError(null); // Clear any previous errors

            // Set local hand order if we already have the game data
            if (games[url_game_id]?.self) {
                setLocalHandOrders(prev => ({ ...prev, [url_game_id]: games[url_game_id].self.hand }));
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
            // Cancel any pending reconnect timer so it doesn't re-subscribe to a
            // game we're navigating away from (which churns the shared socket).
            if (chatChannelRetryTimeout.current) {
                clearTimeout(chatChannelRetryTimeout.current);
                chatChannelRetryTimeout.current = null;
            }
            // Remove subscriptions one channel at a time instead of removeAllChannels().
            // removeAllChannels() calls socket.disconnect() unconditionally, force-closing
            // the websocket (close code 1005) on every game switch — that 1005 then fans
            // out as a CHANNEL_ERROR to the channels being created for the next game.
            // Per-channel removeChannel() instead routes through realtime-js's deferred
            // disconnect (disconnectOnEmptyChannelsAfterMs), which is cancelled as soon as
            // the next game subscribes, so the socket is never bounced during a fast switch.
            // ONLY this context's channels (chat:… and the spectator game-…): the
            // gu-… animation channel is owned and torn down by RealtimeAnimationFeed —
            // removing it here raced its own cleanup/reconnect handling. The pv-…
            // dashboard-cache channel is user-scoped (not game-scoped) and owned by
            // its own effect below, so it must survive game navigation too.
            supabase.getChannels().forEach((channel) => {
                if (channel.topic.includes('gu-') || channel.topic.includes('pv-')) return;
                supabase.removeChannel(channel);
            });
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id, url_game_id]);


    // NOTE: there is deliberately no gu-<game>-<user> subscription here. That
    // personalized channel is owned by RealtimeAnimationFeed (the animation
    // pipeline); a second subscription from this context was pure duplicate
    // socket load — its only events were `private_message` (sender commented out
    // server-side) and `HAND_REARRANGED` (handler was a no-op).

    const subscribeToChatMessages = async (gameId: string) => {
        try {
            // Ensure we have proper auth before subscribing
            await supabase.realtime.setAuth();

            // Subscribe to chat messages for this game
            const chatChannel = supabase.channel(`chat:${gameId}`, {
                config: { private: true }
            });

            chatChannel
                .on('broadcast', { event: 'INSERT' }, (payload) => {
                    handleChatMessage(payload.payload);
                })
                .subscribe((status, err) => {
                    if (status === 'SUBSCRIBED') {
                        chatChannelRetryInterval.current = 500; // Reset retry interval on success
                        if (chatChannelRetryTimeout.current) {
                            clearTimeout(chatChannelRetryTimeout.current);
                            chatChannelRetryTimeout.current = null;
                        }
                    } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                        // Ignore errors for a game we've left: when navigating away the
                        // shared socket is torn down and every channel reports an error;
                        // retrying would re-subscribe to the abandoned game.
                        if (gameIdRef.current !== gameId) {
                            return;
                        }
                        if (err) {
                            console.error(`Chat channel ${status}:`, err);
                        }
                        if (chatChannelRetryTimeout.current) {
                            clearTimeout(chatChannelRetryTimeout.current);
                        }
                        chatChannelRetryTimeout.current = setTimeout(() => {
                            subscribeToChatMessages(gameId).catch(console.error);
                            // Double the interval but cap at MAX_RETRY_INTERVAL
                            chatChannelRetryInterval.current = Math.min(chatChannelRetryInterval.current * 2, MAX_RETRY_INTERVAL);
                        }, chatChannelRetryInterval.current);
                    }
                });
        } catch (error) {
            if (gameIdRef.current !== gameId) {
                return;
            }
            console.error('Error subscribing to chat channel:', error);
            if (chatChannelRetryTimeout.current) {
                clearTimeout(chatChannelRetryTimeout.current);
            }
            chatChannelRetryTimeout.current = setTimeout(() => {
                subscribeToChatMessages(gameId).catch(console.error);
                // Double the interval but cap at MAX_RETRY_INTERVAL
                chatChannelRetryInterval.current = Math.min(chatChannelRetryInterval.current * 2, MAX_RETRY_INTERVAL);
            }, chatChannelRetryInterval.current);
        }
    };

    // mergeHandOrder / reconcileHandMemory / displayedHand / mergeTableBattles all
    // live in src/state/clientReconcile.ts (imported at the top) so they can be
    // unit-tested directly without React.

    // Helper method to update local hand order when game state changes. Sticky:
    // the arrangement memory keeps known slots and only grows with new cards, so a
    // card that's transiently absent (optimistically played then rejected) keeps
    // its slot instead of jumping to the end.
    const updateLocalHandOrder = (gameId: string, newHand: Card[]) => {
        setLocalHandOrders(prev => ({ ...prev, [gameId]: reconcileHandMemory(prev[gameId] || [], newHand) }));
    };


    // Helper method to merge game data while preserving self when not present in new data
    const mergeGameData = (gameId: string, newGameData: any, prevGames: any) => {
        const result = {
            ...newGameData,
            // If self is explicitly provided (including null), use it; otherwise preserve previous self
            self: newGameData.hasOwnProperty('self') ? newGameData.self : prevGames[gameId]?.self,
            // Only authoritative REST loads carry games.version; live broadcast
            // snapshots don't, so keep the last known version rather than clobbering
            // it with undefined (the animation feed seeds its ordering gate from it).
            version: newGameData.version ?? prevGames[gameId]?.version,
        };

        // Merge table_battles to preserve optimistic attacks during out-of-order server responses
        if (newGameData.table_battles && prevGames[gameId]?.table_battles) {
            result.table_battles = mergeTableBattles(prevGames[gameId].table_battles, newGameData.table_battles);
        }

        // If we have both old and new self data with hands, preserve the hand order
        if (newGameData.self && prevGames[gameId]?.self &&
            newGameData.self.hand && prevGames[gameId].self.hand) {
            result.self = {
                ...newGameData.self,
                hand: mergeHandOrder(prevGames[gameId].self.hand, newGameData.self.hand)
            };
        }

        // Update local hand order when game data changes
        if (result.self?.hand) {
            updateLocalHandOrder(gameId, result.self.hand);
        }

        return result;
    };

    // Dashboard live updates (docs/PLAYER_VIEWS.md): subscribe to THIS user's own
    // player_views rows (RLS-enforced) and push each committed masked snapshot
    // straight into `games`. This is the list-level counterpart of
    // RealtimeAnimationFeed's per-game animation stream — full snapshots instead
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
                const decoded = decodePackedGame(hexToBytes(row.view));
                if (!decoded) return;
                const g = decoded.game as PersonalGame;
                // The on-screen game is animation-owned (RealtimeAnimationFeed):
                // pushing its final snapshot here would jump past the in-flight
                // animation. Let that pipeline apply the game being viewed; this
                // subscription keeps every OTHER game in the dashboard live.
                if (g.id === urlGameIdRef.current) return;
                setGames(prev => ({ ...prev, [g.id]: mergeGameData(g.id, { ...g, self: (g as any).self ?? null }, prev) }));
            } catch { /* unreadable snapshot — ignore; the next fetch resyncs */ }
        };

        const pgChanges = { schema: 'public', table: 'player_views', filter: `player_id=eq.${user_id}` } as const;

        // setAuth() hands Realtime the caller's JWT so postgres_changes applies
        // player_views' RLS per row. NOT a `private` broadcast channel: for
        // postgres_changes the source table's RLS is the gate (the row filter
        // below is enforced server-side), not a realtime.messages topic policy —
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
                    // The old row carries only the replica-identity (PK) columns —
                    // game_id + player_id — which is all we need to drop it.
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
        // create now returns the caller's PACKED view buffer (like get_game) —
        // decode it with the shared codec, the same path loadGame uses. A JSON
        // body is the legacy fallback. The server persists the game to the DB in
        // the background AFTER responding, so this returns as soon as the lobby is
        // built (no create_game round-trip on the critical path).
        const { data, error } = await supabase.functions.invoke('create', { body: {} });
        if (error) throw error;

        let game: PersonalGame;
        if (typeof Blob !== 'undefined' && data instanceof Blob) {
            const decoded = decodePackedGame(new Uint8Array(await data.arrayBuffer()));
            if (!decoded) throw new Error('create: unreadable packed response');
            game = { ...(decoded.game as PersonalGame), self: (decoded.game as PersonalGame).self ?? null };
        } else {
            const fetched: any = data;
            if (!fetched || fetched.error || !fetched.id) throw new Error(fetched?.error || 'create failed');
            game = { ...fetched, self: fetched.self ?? null };
        }

        setGameId(game.id);
        setGames(prev => ({ ...prev, [game.id]: mergeGameData(game.id, game, prev) }));
        // Subscribe to the new game's chat (the gu- animation channel is owned by
        // RealtimeAnimationFeed).
        subscribeToChatMessages(game.id).catch(console.error);
        return { game_id: game.id };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const joinGame = useCallback((gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', {
            type: 'join',
            game_id: gameId,
        }, {
            onSuccess: (data) => {
                setGameId(data.data.id);
                // Remove from spectator mode when joining
                setSpectatorGames(prev => {
                    const newSet = new Set(prev);
                    newSet.delete(gameId);
                    return newSet;
                });

                // Clean up old game channel (for spectators) and switch to game-user channel
                const oldChannelName = `game-${gameId}`;
                const channels = supabase.getChannels();
                const oldChannel = channels.find(channel => channel.topic === oldChannelName);
                if (oldChannel) {
                    supabase.removeChannel(oldChannel);
                }

                // Subscribe to the game's chat (the gu- animation channel is
                // owned by RealtimeAnimationFeed)
                subscribeToChatMessages(data.data.id).catch(console.error);
                // Load chat history with game data
                loadChatHistory(data.data.id, data.data).catch(console.error);
            }
        })
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // start / add-bot / exit / continue are one consolidated `meta` endpoint
    // (dispatched on `type`) — fewer functions, faster deploys.
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
            onSuccess: (data) => {
                // If user removed themselves (not a bot), mark as spectating and switch channels
                if (!botId) {
                    setSpectatorGames(prev => new Set(prev).add(gameId));

                    // Clean up old game-user channel and switch to game channel for spectators
                    const oldChannelName = `gu-${gameId}-${userIdRef.current}`;
                    const channels = supabase.getChannels();
                    const oldChannel = channels.find(channel => channel.topic === oldChannelName);
                    if (oldChannel) {
                        supabase.removeChannel(oldChannel);
                    }

                    // Subscribe to game channel for spectators
                    supabase.realtime.setAuth().then(() => {
                        const gameChannel = supabase.channel(`game-${gameId}`, {
                            config: { private: true }
                        });
                        // Spectators currently don't need to listen to any events
                        // All game state is included in animation events
                        gameChannel.subscribe((status, err) => status === 'SUBSCRIBED'
                            ? console.log('Connected to game channel:', `game-${gameId}`)
                            : console.error('Game channel error:', err));
                    });
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
    // own already-masked view straight from player_views — a plain indexed RLS
    // SELECT, no get_game edge round-trip. RLS scopes it to the caller, and the
    // (game_id, player_id) PK means at most one row. Returns null for a spectator
    // (no row), a cache miss (game predating the cache), or any failure — the
    // caller then falls back to get_game, which also masks spectator views.
    const loadGameFromCache = async (gameId: string): Promise<PersonalGame | null> => {
        try {
            if (!userIdRef.current) return null;
            const { data, error } = await supabase
                .from('player_views')
                .select('view')
                .eq('game_id', gameId)
                .maybeSingle();
            if (error || !(data as any)?.view) return null;
            const decoded = decodePackedGame(hexToBytes((data as any).view));
            if (!decoded) return null;
            const g = decoded.game as PersonalGame;
            return { ...g, self: (g as any).self ?? null };
        } catch {
            return null;
        }
    };

    const loadGameInternal = async (gameId: string): Promise<{ game_id: string }> => {
        try {
            // Player fast path: the caller's own masked view from the
            // player_views cache. Falls back to get_game for spectators / a cold
            // cache (the authoritative rebuild path, which also masks spectators).
            let game: PersonalGame | null = await loadGameFromCache(gameId);

            if (!game) {
                // The volatile game state lives in the packed kernel blob. With
                // packed:true a DEALT game comes back as binary (roster JSON +
                // the caller's kernel-masked view blob) that materializes into a
                // PersonalGame right here — the render boundary; a lobby / legacy
                // row falls back to the old personalize_game JSON. functions-js
                // hands octet-stream responses over as a Blob, JSON as a parsed
                // object, so the response type is the format switch.
                const { data, error } = await supabase.functions.invoke('get_game', {
                    body: { game_id: gameId, packed: true },
                });

                if (!error && typeof Blob !== 'undefined' && data instanceof Blob) {
                    const decoded = decodePackedGame(new Uint8Array(await data.arrayBuffer()));
                    if (!decoded) {
                        throw new Error(`Game ${gameId}: unreadable packed game response`);
                    }
                    // decodePackedGame stamps game.version from the envelope; a
                    // spectator gets no `self` (null below), same as the JSON path.
                    game = { ...(decoded.game as PersonalGame), self: (decoded.game as PersonalGame).self ?? null };
                } else {
                    // Legacy JSON: has `self` for a player, no `self` for a
                    // spectator (self stays null). Typed loosely like the old
                    // direct reads — PersonalGame.self is non-null in the type but
                    // null at runtime for spectators.
                    const fetched: any = data;
                    if (error || !fetched || fetched.error) {
                        throw new Error(fetched?.error || `Game ${gameId} not found`);
                    }
                    game = { ...fetched, self: fetched.self ?? null };
                }
            }

            // Both the cache hit and each fetch branch above assign or throw, so
            // this only narrows the type (game is non-null here).
            if (!game) throw new Error(`Game ${gameId} not found`);

            if (game.self) {
                // Re-apply the local player's unconfirmed optimistic cards onto
                // the authoritative state so a reconnect resync doesn't make a
                // just-played card vanish-then-reappear (Q7). No-op on a normal
                // load (nothing optimistic pending).
                applyOptimisticOverlay(game);
            }

            setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, game, prev) }));
            joinOrSubscribe(game);

            // Trigger the bot loop only if the caller is a player in a game with
            // AI players. Fire and forget - don't block UI rendering.
            if (game.self && game.players.some(player => player.is_ai)) {
                supabase.functions.invoke('action', { body: { game_id: gameId, type: 'bump' } }).catch(() => { });
            }

            return { game_id: gameId };

        } catch (error) {
            throw error;
        }
    };

    const joinOrSubscribe = (game: PersonalGame) => {
        const gameId = game.id;

        // Set game_id state and game data first, then load chat history
        setGameId(gameId);
        //setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, game, prev) }));

        // Load chat history with game data
        loadChatHistory(gameId, game).catch(console.error);

        if (game.self) {
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
        if (!isSpectating && game.status === GAME_STATUS.WAITING && game.players.length < MAX_PLAYERS) {
            // Auto-join only if not intentionally spectating
            joinGame(gameId).catch(console.error);
        } else {
            // Subscribe as spectator
            supabase.realtime.setAuth().then(() => {
                const gameChannel = supabase.channel(`game-${gameId}`, {
                    config: { private: true }
                });
                // Spectators currently don't need to listen to any events
                // All game state is included in animation events
                gameChannel.subscribe((status, err) => status === 'SUBSCRIBED'
                    ? console.log('Connected to game channel:', `game-${gameId}`)
                    : console.error('Game channel error:', err));

                // Subscribe to chat messages for spectators too
                subscribeToChatMessages(gameId).catch(console.error);
            });
        }
    }

    const loadChatHistory = async (gameId: string, gameData?: PersonalGame): Promise<void> => {
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

    const attack = useCallback((cards: Card[], applyOptimistic: () => boolean = () => true, wire?: Uint8Array): Promise<{ game_id: string }> => {
        // The game this move targets: captured ONCE at tap time, so the deferred
        // optimistic patch below applies to the same game the request went to
        // even if the user navigates during the animation.
        const gid = activeGameIdRef.current!;
        // Fire the server request FIRST — the server is authoritative and rejects an
        // illegal move, so the round-trip isn't gated on local validation. The body
        // is the packed awire buffer: the caller-supplied bytes (already validated
        // against guards.wasm) or a fresh encode for direct callers.
        const promise = invokePackedAction(gid, wire ?? encodeAction({ kind: 'attack', cards }));

        // Optimistic game state update after animation completes — but only if the
        // caller's validation (evaluated by ANIMATION_TIME, when this fires) agrees the
        // move was legal. An invalid move gets no optimistic state to roll back.
        // Everything is derived inside the updater from prev: this fires up to
        // ANIMATION_TIME after the tap, and a broadcast can commit fresher state in
        // that window — deriving from the render-time `games` closure would write
        // that stale table/hand back over it.
        setTimeout(() => {
            if (!applyOptimistic()) return;
            setGames(prev => {
                const g: PersonalGame = prev[gid];
                if (!g) return prev;
                return {
                    ...prev,
                    [gid]: {
                        ...g,
                        table_battles: [...g.table_battles, ...cards.map(card => ({ attack: card, defense: null }))],
                        self: { ...g.self, hand: g.self.hand.filter(card => !cards.some(c => card_comp(c, card))) }
                    }
                };
            });
            // Hand order is derived from self.hand by the displayedHand selector,
            // so the optimistic removal above is reflected automatically.

        }, ANIMATION_TIME);

        return promise;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const pass = useCallback((cards: Card[], applyOptimistic: () => boolean = () => true, wire?: Uint8Array): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokePackedAction(gid, wire ?? encodeAction({ kind: 'pass', cards }));

        // Optimistic game state update after animation completes. Derived inside
        // the updater from prev — see attack for why the closure state is stale.
        setTimeout(() => {
            if (!applyOptimistic()) return;
            setGames(prev => {
                const g: PersonalGame = prev[gid];
                if (!g) return prev;
                return {
                    ...prev,
                    [gid]: {
                        ...g,
                        table_battles: [...g.table_battles, ...cards.map(card => ({ attack: card, defense: null }))],
                        self: { ...g.self, hand: g.self.hand.filter(card => !cards.some(c => card_comp(c, card))) },
                        defender: get_next_player_index(g, g.defender)
                    }
                };
            });
            // Hand order derives from self.hand (see displayedHand selector).

        }, ANIMATION_TIME);

        return promise;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const pickup = useCallback((applyOptimistic: () => boolean = () => true, wire?: Uint8Array): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokePackedAction(gid, wire ?? encodeAction({ kind: 'pickup' }));

        // Optimistic game state update after animation completes. Derived inside
        // the updater from prev — see attack for why the closure state is stale.
        setTimeout(() => {
            if (!applyOptimistic()) return;
            setGames(prev => {
                const g: PersonalGame = prev[gid];
                if (!g) return prev;

                // The kernel rotates AFTER refill_player_hands, which can
                // eliminate a hand-emptied seat when the stock runs dry — a
                // pre-refill rotation would then point at a seat the kernel
                // skips. The rotation is exact iff no OTHER in-play seat can
                // be eliminated by the refill (they all still hold cards; we
                // are the picker and gain the table cards). Otherwise leave
                // the seats to the authoritative broadcast.
                const selfIndex = g.players.findIndex(p => p.player_id === g.self?.player_id);
                const rotationIsExact = g.players.every((p, i) =>
                    i === selfIndex || p.status !== PLAYER_STATUS.IN || (p.hand_length ?? 0) > 0);
                const next_first_attacker = rotationIsExact
                    ? get_next_player_index(g, g.defender) : g.first_attacker;
                const next_defender = rotationIsExact
                    ? get_next_player_index(g, next_first_attacker) : g.defender;

                // Collect all cards from the table (both attacks and defenses)
                const allTableCards = g.table_battles.flatMap(battle =>
                    battle.defense ? [battle.attack, battle.defense] : [battle.attack]
                );

                return {
                    ...prev,
                    [gid]: {
                        ...g,
                        table_battles: [],
                        self: {
                            ...g.self,
                            hand: [...g.self.hand, ...allTableCards]
                        },
                        first_attacker: next_first_attacker,
                        defender: next_defender
                    }
                };
            });
            // Picked-up cards appear via self.hand; the displayedHand selector
            // appends any new cards to the end of the arrangement automatically.

        }, ANIMATION_TIME);

        return promise;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const cover = useCallback((coverCards: Card[], attackCards: Card[], applyOptimistic: () => boolean = () => true, wire?: Uint8Array): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokePackedAction(gid,
            wire ?? encodeAction({ kind: 'cover', cards: coverCards, attack_cards: attackCards }));

        // Optimistic game state update after animation completes. Derived inside
        // the updater from prev — see attack for why the closure state is stale.
        setTimeout(() => {
            if (!applyOptimistic()) return;
            setGames(prev => {
                const g: PersonalGame = prev[gid];
                if (!g) return prev;

                const updatedTableBattles = g.table_battles.map(battle => {
                    const attackIndex = attackCards.findIndex(card =>
                        card_comp(card, battle.attack)
                    );
                    if (attackIndex !== -1) {
                        return { ...battle, defense: coverCards[attackIndex] };
                    }
                    return battle;
                });

                return {
                    ...prev,
                    [gid]: {
                        ...g,
                        table_battles: updatedTableBattles,
                        self: { ...g.self, hand: g.self.hand.filter(card => !coverCards.some(c => card_comp(c, card))) }
                    }
                };
            });
            // Hand order derives from self.hand (see displayedHand selector).

        }, ANIMATION_TIME);

        return promise;
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
        const previousName = gamesRef.current[gameId]?.name;

        setGames(prev => ({
            ...prev,
            [gameId]: {
                ...prev[gameId],
                name: name
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
                    name: previousName
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
        const previousPlayers = gamesRef.current[gameId]?.players ? [...gamesRef.current[gameId].players] : [];
        if (previousPlayers.length === 0) {
            return Promise.reject(new Error(`Cannot rearrange players`));
        }
        const rearrangedPlayers = playerIds.map(playerId =>
            previousPlayers.find(p => p.player_id === playerId)!
        );
        setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], players: rearrangedPlayers } }));

        const revert = () => {
            if (previousPlayers.length === 0) {
                return;
            }
            setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], players: previousPlayers } }));
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
        const previousHand = gamesRef.current[gameId]?.self?.hand ? [...gamesRef.current[gameId].self.hand] : [];

        if (previousHand.length === 0) {
            return Promise.reject(new Error(`Cannot rearrange hand`));
        }
        const rearrangedHand = cardIndices.map(index => previousHand[index]);
        setGames(prev => ({
            ...prev, [gameId]: {
                ...prev[gameId],
                self: { ...prev[gameId]?.self, hand: rearrangedHand }
            }
        }));

        const revert = () => {
            if (previousHand.length === 0) {
                return;
            }
            setGames(prev => ({
                ...prev, [gameId]: {
                    ...prev[gameId],
                    self: { ...prev[gameId]?.self, hand: previousHand }
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
        // player_views cache (docs/PLAYER_VIEWS.md) — no edge function, no cold
        // start, no per-viewer masking on read (rows are masked at write time).
        // Each row's `view` is the caller's packed single-game envelope,
        // materialized here by the shared decodePackedGame. player_views is kept
        // complete by commit_game / create_game, so there is no fallback: an
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
            const games: { [key: string]: PersonalGame } = {};
            for (const row of rows ?? []) {
                try {
                    const decoded = decodePackedGame(hexToBytes((row as any).view));
                    if (!decoded) continue;
                    const g = decoded.game as PersonalGame;
                    games[g.id] = { ...g, self: (g as any).self ?? null };
                } catch { /* skip an unreadable row */ }
            }
            setGames(prev => ({ ...prev, ...games }));
        } catch (e) {
            console.error('Error in getUserGames:', e);
        }
    };

    const continueGame = useCallback((gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('meta', { type: 'continue', game_id: gameId });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const invokeGameFunctions = async <T = any>(
        functionName: string,
        body: any = {},
        options: {
            onSuccess?: (data: T) => void;
            onError?: (error: any) => void;
        } = {}
    ): Promise<{ game_id: string }> => {
        try {
            const data = await supabase.functions.invoke(functionName, { body })

            if (!data.data || !data.data.id) {
                throw new Error(`Invalid response from ${functionName}: missing game ID`);
            }

            const game_id = data.data.id;

            options.onSuccess?.(data as T);

            return { game_id };

        } catch (error) {
            options.onError?.(error);
            throw error;
        }
    }

    // The packed move transport (docs/PACKED_WIRE_CUTOVER.md): POST the awire
    // bytes — the exact buffer guards.wasm validated — wrapped in the binary
    // request envelope. functions-js only passes a body through with
    // Content-Type: application/octet-stream when it is a Blob or an
    // ArrayBuffer (a Uint8Array would be JSON.stringified — see
    // @supabase/functions-js FunctionsClient.invoke), so the envelope rides in
    // a Blob; octet-stream responses come back as a Blob too.
    // 'bump' and all meta ops stay JSON via invokeGameFunctions.
    const invokePackedAction = async (gameId: string, wire: Uint8Array): Promise<{ game_id: string }> => {
        const req = encodeActionRequest(gameId, wire);
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
        if (resp.status === ACTION_STATUS.REJECTED) {
            // Console-only diagnostics: callers revert the optimistic state.
            throw new Error(rejectMessage(resp.rejectCode));
        }
        // APPLIED — or MOOT (the move lost the end-game race, a no-op): both
        // resolve as success, mirroring the old JSON path's data.id check.
        return { game_id: gameId };
    };

    const updateGameState = useCallback((gameId: string, gameState: any) => {
        setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, gameState, prev) }));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const setLocalHandOrder = useCallback((order: Card[]) => {
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
        game: games[active_game_id!],
        games,
        gameLoadError,
        chatMessages: chatMessages[active_game_id!] || [],
        // The rendered hand: authoritative self.hand, deduped and ordered by the
        // sticky arrangement memory. Guarantees no duplicates and no on-table
        // cards in the hand, and keeps a rejected card in its original slot.
        localHandOrder: displayedHand(localHandOrders[active_game_id!] || [], games[active_game_id!]?.self?.hand || []),
    }), [games, active_game_id, gameLoadError, chatMessages, localHandOrders]);

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
    // The optional `applyOptimistic` thunk gates the deferred optimistic local-state
    // patch: callers fire the request before validating, then have the patch apply
    // only if validation passed (evaluated at ANIMATION_TIME). Defaults to always-on.
    // The optional `wire` is the move's awire buffer (encodeAction) — passed by
    // callers that already validated those bytes so the POST body is bit-identical;
    // encoded on the spot when absent.
    attack: (cards: Card[], applyOptimistic?: () => boolean, wire?: Uint8Array) => Promise<{ game_id: string }>;
    pass: (cards: Card[], applyOptimistic?: () => boolean, wire?: Uint8Array) => Promise<{ game_id: string }>;
    pickup: (applyOptimistic?: () => boolean, wire?: Uint8Array) => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[], applyOptimistic?: () => boolean, wire?: Uint8Array) => Promise<{ game_id: string }>;
    good: () => Promise<{ game_id: string }>;
    sendMessage: (message: string) => Promise<void>;
    getUserGames: () => Promise<void>;
    updateGameState: (gameId: string, gameState: any) => void;
    updateGameName: (gameId: string, name: string) => Promise<{ game_id: string }>;
    rearrangePlayer: (gameId: string, playerIds: string[]) => Promise<{ game_id: string }>;
    rearrangeHand: (gameId: string, cardIndices: number[]) => Promise<{ game_id: string }>;
    continueGame: (gameId: string) => Promise<{ game_id: string }>;
    /** Refetch authoritative game state over REST. Used to resync after a
     *  realtime reconnect, where broadcasts missed during the gap are lost. */
    loadGame: (gameId: string) => Promise<{ game_id: string }>;
    setLocalHandOrder: (order: Card[]) => void;
}

interface ServerStateType {
    game_id: string | null;
    game: PersonalGame | null;
    games: { [key: string]: PersonalGame };
    gameLoadError: string | null;
    chatMessages: any[];
    localHandOrder: Card[];
}

type ServerContextType = ServerActionsType & ServerStateType;

/** Actions only — the value is referentially stable for the provider's whole
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
// read. updateGameState really updates (plain replacement — no optimistic
// merging here), which is what lets the real animation pipeline drive the
// replay: each synthesized event commits its game_state snapshot exactly
// like a live broadcast would. Every server method is inert.
export const ReplayServerProvider = ({ gameId, initialGame, children }: {
    gameId: string,
    initialGame: PersonalGame,
    children: React.ReactNode,
}) => {
    const [games, setGames] = useState<{ [key: string]: PersonalGame }>({ [gameId]: initialGame });

    const updateGameState = useCallback((gid: string, gameState: any) => {
        setGames(prev => ({ ...prev, [gid]: gameState }));
    }, []);

    // The tutorial seats a real `self`, so the live ActionButtons + drag system
    // need a hand to render. Mirror the current self hand (a plain replay's
    // viewer has none, so this stays []). Reordering is a no-op here — the
    // tutorial doesn't need drag-to-rearrange, only drag-to-play.
    const localHandOrder = games[gameId]?.self?.hand ?? initialGame.self?.hand ?? [];

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
        game: games[gameId] ?? initialGame,
        games,
        gameLoadError: null,
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
