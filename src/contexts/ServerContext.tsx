import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { Card, PersonalGame, PublicGame, GAME_STATUS, STRATEGY_KEY } from '@shared/types.ts';
import supabase from '../backend/Connector';
import { useParams } from 'next/navigation';
import { useAuth } from './AuthContext';
import { MAX_PLAYERS } from '@shared/constants.ts';
import { get_next_player_index, card_comp } from '@shared/common_utils.ts';
import { ANIMATION_TIME } from '../constants/constants';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { cardKey, mergeHandOrder, reconcileHandMemory, displayedHand, mergeTableBattles, applyOverlayEntries } from '../state/clientReconcile';

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

const handsQuery =
    `game_id,
hand,
awaiting_attack,
games!inner (
    defender,
    deck_length,
    first_attacker,
    flipped,
    id,
    name,
    players,
    power_suit,
    status,
    table_battles,
    discard_pile_length,
    updated_at,
    elimination_order,
    good_timestamp,
    good_players,
    version
)`;

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
    useEffect(() => { gamesRef.current = games; }, [games]);
    useEffect(() => { userIdRef.current = user_id; }, [user_id]);
    useEffect(() => { activeGameIdRef.current = active_game_id; }, [active_game_id]);
    useEffect(() => { spectatorGamesRef.current = spectatorGames; }, [spectatorGames]);

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
            // removing it here raced its own cleanup/reconnect handling.
            supabase.getChannels().forEach((channel) => {
                if (channel.topic.includes('gu-')) return;
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

    const createGame = useCallback((): Promise<{ game_id: string }> => {
        return invokeGameFunctions('create', {}, {
            onSuccess: (data) => {
                setGameId(data.data.id);
                setGames(prev => ({ ...prev, [data.data.id]: mergeGameData(data.data.id, data.data, prev) }));
                // Subscribe to the new game's chat (the gu- animation channel is
                // owned by RealtimeAnimationFeed)
                subscribeToChatMessages(data.data.id).catch(console.error);
            }
        });
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

    const loadGameInternal = async (gameId: string): Promise<{ game_id: string }> => {
        try {
            // First try to get game data if user is a player (only if user_id exists)
            const user_id = userIdRef.current;
            if (user_id) {
                const { data: playerData, error: playerError } = await supabase
                    .from('player_hands')
                    .select(handsQuery)
                    .eq('game_id', gameId)
                    .eq('player_id', user_id)
                    .single();

                if (playerError) {
                }

                if (playerData && !playerError) {
                    // User is in the game - return personalized data
                    const game = playerData.games as unknown as PublicGame;

                    const selfPlayer = game.players.find((player) => player.player_id === user_id);

                    if (selfPlayer) {
                        const personalizedGame: PersonalGame = {
                            ...game,
                            self: {
                                ...selfPlayer,
                                player_id: user_id,
                                hand: playerData.hand,
                                awaiting_attack: playerData.awaiting_attack,
                                strategy_key: STRATEGY_KEY.HUMAN
                            }
                        };

                        // Re-apply the local player's unconfirmed optimistic cards onto
                        // the authoritative state so a reconnect resync doesn't make a
                        // just-played card vanish-then-reappear (Q7). No-op on a normal
                        // load (nothing optimistic pending).
                        applyOptimisticOverlay(personalizedGame);

                        setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, personalizedGame, prev) }));
                        joinOrSubscribe(personalizedGame);

                        // Trigger bot loop only if there are AI players in the game
                        // Fire and forget - don't block UI rendering
                        if (game.players.some(player => player.is_ai)) {
                            supabase.functions.invoke('action', { body: { game_id: gameId, type: 'bump' } }).catch(botError => {
                            });
                        }

                        return { game_id: gameId };
                    }
                }
            }

            // User is not in the game - try to get public game data for spectating
            const { data: publicData, error: publicError } = await supabase
                .from('games')
                .select('*')
                .eq('id', gameId)
                .single();

            if (publicError) {
                throw new Error(`Game ${gameId} not found`);
            }

            // Create public game for spectating
            const publicGame: PersonalGame = {
                ...publicData,
                self: null // No self data for spectators
            };

            setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, publicGame, prev) }));
            joinOrSubscribe(publicGame);
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

    const attack = useCallback((cards: Card[], applyOptimistic: () => boolean = () => true): Promise<{ game_id: string }> => {
        // The game this move targets: captured ONCE at tap time, so the deferred
        // optimistic patch below applies to the same game the request went to
        // even if the user navigates during the animation.
        const gid = activeGameIdRef.current!;
        // Fire the server request FIRST — the server is authoritative and rejects an
        // illegal move, so the round-trip isn't gated on local validation.
        const promise = invokeGameFunctions('action', {
            type: 'attack',
            game_id: gid,
            cards: cards,
        });

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

    const pass = useCallback((cards: Card[], applyOptimistic: () => boolean = () => true): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokeGameFunctions('action', {
            type: 'pass',
            game_id: gid,
            cards: cards,
        });

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

    const pickup = useCallback((applyOptimistic: () => boolean = () => true): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokeGameFunctions('action', {
            type: 'pickup',
            game_id: gid,
        });

        // Optimistic game state update after animation completes. Derived inside
        // the updater from prev — see attack for why the closure state is stale.
        setTimeout(() => {
            if (!applyOptimistic()) return;
            setGames(prev => {
                const g: PersonalGame = prev[gid];
                if (!g) return prev;

                const next_first_attacker = get_next_player_index(g, g.defender);
                const next_defender = get_next_player_index(g, next_first_attacker);

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

    const cover = useCallback((coverCards: Card[], attackCards: Card[], applyOptimistic: () => boolean = () => true): Promise<{ game_id: string }> => {
        const gid = activeGameIdRef.current!; // see attack
        // Server request first (see attack); optimistic patch gated on validity.
        const promise = invokeGameFunctions('action', {
            type: 'cover',
            game_id: gid,
            cover_cards: coverCards,
            attack_cards: attackCards,
        });

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
        return invokeGameFunctions('action', {
            type: 'good',
            game_id: activeGameIdRef.current!,
        });
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
        // This needs to also throw in status at least
        try {
            const user_id = userIdRef.current;
            if (!user_id) {
                return;
            }

            // Direct SQL query that respects RLS policies
            // Join player_hands (user can only see their own) with games (public data)
            const { data, error } = await supabase
                .from('player_hands')
                .select(handsQuery)
                .eq('player_id', user_id)
                .order('games(updated_at)', { ascending: false });

            if (error) {
                console.error('Error fetching user games:', error);
                return;
            }

            const games: { [key: string]: PersonalGame } = {};

            for (const playerHand of data) {
                try {
                    // Fuck you I know this will be a public game type
                    const game = playerHand.games as unknown as PublicGame;
                    const selfPlayer = game.players.find((player) => player.player_id === user_id);
                    if (!selfPlayer) {
                        continue;
                    }
                    games[game.id] = {
                        ...game,
                        self: { ...selfPlayer, player_id: user_id, hand: playerHand.hand, awaiting_attack: playerHand.awaiting_attack, strategy_key: STRATEGY_KEY.HUMAN }// as unknown as PrivatePlayer
                    };
                } catch (gameError) {
                    // ignore individual game errors
                }
            }

            setGames(prev => ({ ...prev, ...games }));

        } catch (error) {
            console.error('Error in getUserGames:', error);
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
    attack: (cards: Card[], applyOptimistic?: () => boolean) => Promise<{ game_id: string }>;
    pass: (cards: Card[], applyOptimistic?: () => boolean) => Promise<{ game_id: string }>;
    pickup: (applyOptimistic?: () => boolean) => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[], applyOptimistic?: () => boolean) => Promise<{ game_id: string }>;
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
