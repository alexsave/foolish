import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, PublicPlayer, PersonalGame, PublicGame, PRIVATE_EVENT_TYPE } from '../common/types';
import supabase from '../backend/Connector';
import { useParams } from 'react-router-dom';
import { useAuth } from './AuthContext';
import { MAX_PLAYERS } from '../common/constants';
import { get_next_player_index, canCover, card_comp } from '../common/common_utils';
import { ANIMATION_TIME } from '../constants/constants';

const ServerContext = createContext<ServerContextType | null>(null);

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
    updated_at
)`;

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const { user_id } = useAuth();
    const url_game_id = useParams().game_id?.toLowerCase();
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

    // Local hand order state - keyed by game_id
    // Thinking we just need one tbh
    const [localHandOrders, setLocalHandOrders] = useState<{ [key: string]: Card[] }>({});

    const [localHand, setLocalHand] = useState<Card[]>([]);

    // Use ref to avoid closure issues in WebSocket handler
    const gameIdRef = useRef<string | null>(null);
    const userNamesRef = useRef<{ [userId: string]: string }>({});

    // Use ref to prevent duplicate user effect executions
    const prevUserRef = useRef<string | null>(null);

    // Simple reconnection state
    const gameChannelRetryInterval = useRef(1000); // Start with 1 second
    const chatChannelRetryInterval = useRef(1000); // Start with 1 second

    useEffect(() => {
        if (url_game_id) {
            gameIdRef.current = url_game_id;
            setGameId(url_game_id);
            setGameLoadError(null); // Clear any previous errors
            // iffy on this call. Just load the specific game
            getUserGames();
            // The problem is that getUserGames and loadGame are incompatible
            if ( games[url_game_id]?.self){
                setLocalHandOrders(prev => ({ ...prev, [url_game_id]: games[url_game_id].self.hand }));
            }
            if (!games[url_game_id]/* && !loading*/) {
                //setLoading(true);
                loadGame(url_game_id).catch(error => {
                    console.log('Game not found in URL:', error.message);
                    setGameLoadError(url_game_id); // Set error for this specific game
                });
            }
        }
    }, [url_game_id]);



    // Keep ref in sync with state


    useEffect(() => {
        // Skip if user hasn't actually changed
        if (prevUserRef.current === user_id) {
            //return;
        }

        prevUserRef.current = user_id;

        if (user_id) {
            // Call getUserGames to console.log the player's games
            getUserGames();
        }

        // cleanup realtime subscriptions
        return () => {
            // Remove all realtime subscriptions
            supabase.removeAllChannels();
        };
    }, [user_id]);


    const subscribeToGame = async (gameId: string) => {
        try {
            // Ensure we have proper auth before subscribing
            await supabase.realtime.setAuth();

            // Subscribe to personalized game-user channel for non-animation game updates  
            const gameUserChannel = supabase.channel(`gu-${gameId}-${user_id}`, {
                config: { private: true }
            });

            gameUserChannel
                .on('broadcast', { event: 'private_message' }, (payload) => {
                    handleGameMessage(payload.payload, 'private_message');
                })
                .on('broadcast', { event: 'HAND_REARRANGED' }, (payload) => {
                    handleGameMessage(payload.payload, 'HAND_REARRANGED');
                })
                .subscribe((status, err) => {
                    if (status === 'SUBSCRIBED') {
                        console.log('Connected to game-user channel:', `gu-${gameId}-${user_id}`);
                        gameChannelRetryInterval.current = 1000; // Reset retry interval on success
                    } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                        console.error('Game-user channel error:', err);
                        console.log(`Retrying game channel connection in ${gameChannelRetryInterval.current}ms`);
                        setTimeout(() => {
                            subscribeToGame(gameId).catch(console.error);
                            gameChannelRetryInterval.current *= 2; // Double the interval
                        }, gameChannelRetryInterval.current);
                    }
                });
        } catch (error) {
            console.error('Error subscribing to game channel:', error);
            setTimeout(() => {
                subscribeToGame(gameId).catch(console.error);
                gameChannelRetryInterval.current *= 2; // Double the interval
            }, gameChannelRetryInterval.current);
        }
    };

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
                        //console.log('Connected to chat channel:', `chat:${gameId}`);
                        chatChannelRetryInterval.current = 1000; // Reset retry interval on success
                    } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                        console.error('Chat channel error:', err);
                        console.log(`Retrying chat channel connection in ${chatChannelRetryInterval.current}ms`);
                        setTimeout(() => {
                            subscribeToChatMessages(gameId).catch(console.error);
                            chatChannelRetryInterval.current *= 2; // Double the interval
                        }, chatChannelRetryInterval.current);
                    }
                });
        } catch (error) {
            console.error('Error subscribing to chat channel:', error);
            setTimeout(() => {
                subscribeToChatMessages(gameId).catch(console.error);
                chatChannelRetryInterval.current *= 2; // Double the interval
            }, chatChannelRetryInterval.current);
        }
    };

    const handleGameMessage = (message: any, source: string = 'unknown') => {
        // Handle both old format (message.game_id) and new format (message.game.id)
        // Also handle nested message format from broadcastToGame
        let actualMessage = message;
        let messageGameId = message.game?.id || message.game_id;

        // Check if this is a nested message from broadcastToGame
        if (message.message && typeof message.message === 'object') {
            actualMessage = message.message;
            messageGameId = actualMessage.game?.id || message.game_id;
        }

        if (!messageGameId || !gameIdRef.current || messageGameId !== gameIdRef.current) {
            return;
        }

        console.log(`[${source.toUpperCase()}] Game message received:`, actualMessage);

        const gameData = actualMessage.game || message.game;

        // Handle non-animation messages (private messages, direct responses, etc.)
        if (actualMessage.type === PRIVATE_EVENT_TYPE.REQUEST_FIRST_ATTACK || 
            actualMessage.type === PRIVATE_EVENT_TYPE.PLAYER_HAND) {
            // These are private messages that don't need game state updates
            console.log('Private message received:', actualMessage.message);
        } else if (gameData) {
            // For any other message type that includes game data, update the game state
            // This handles cases like direct function invocation responses
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        }
    };

    // Helper method to merge hands while preserving local card order
    const mergeHandOrder = (oldHand: Card[], newHand: Card[]): Card[] => {
        if (!oldHand || !newHand) return newHand || [];
        
        // Create a map of card positions in the old hand for quick lookup
        const oldCardPositions = new Map<string, number>();
        oldHand.forEach((card, index) => {
            const key = `${card.suit}-${card.value}`;
            oldCardPositions.set(key, index);
        });
        
        // Create a set of new cards for quick lookup
        const newCardSet = new Set(newHand.map(card => `${card.suit}-${card.value}`));
        
        // Keep existing cards in their current positions
        const preservedCards: Card[] = [];
        const preservedPositions = new Set<number>();
        
        oldHand.forEach((card, oldIndex) => {
            const key = `${card.suit}-${card.value}`;
            if (newCardSet.has(key)) {
                preservedCards.push(card);
                preservedPositions.add(oldIndex);
            }
        });
        
        // Find new cards (cards in newHand but not in oldHand)
        const newCards = newHand.filter(card => {
            const key = `${card.suit}-${card.value}`;
            return !oldCardPositions.has(key);
        });
        
        // Combine preserved cards with new cards (new cards go to the end)
        return [...preservedCards, ...newCards];
    };

    // Helper method to update local hand order when game state changes
    const updateLocalHandOrder = (gameId: string, newHand: Card[]) => {
        setLocalHandOrders(prev => {
            const currentOrder = prev[gameId] || [];
            
            // If we don't have a previous order, just use the new hand
            if (currentOrder.length === 0) {
                return { ...prev, [gameId]: newHand };
            }
            
            // Otherwise, preserve existing order and add new cards to the end
            const mergedOrder = mergeHandOrder(currentOrder, newHand);
            return { ...prev, [gameId]: mergedOrder };
        });
    };

    // Helper method to merge game data while preserving self when not present in new data
    const mergeGameData = (gameId: string, newGameData: any, prevGames: any) => {
        const result = {
            ...newGameData,
            // If self is explicitly provided (including null), use it; otherwise preserve previous self
            self: newGameData.hasOwnProperty('self') ? newGameData.self : prevGames[gameId]?.self
        };
        
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

    const createGame = (): Promise<{ game_id: string }> => {
        return invokeGameFunctions('create', {}, {
            onSuccess: (data) => {
                setGameId(data.data.id);
                setGames(prev => ({ ...prev, [data.data.id]: mergeGameData(data.data.id, data.data, prev) }));
                // Subscribe to the new game's channel and chat messages
                subscribeToGame(data.data.id).catch(console.error);
                subscribeToChatMessages(data.data.id).catch(console.error);
            }
        });
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('join', {
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
                
                // Subscribe to the game's channel and chat messages
                subscribeToGame(data.data.id).catch(console.error);
                subscribeToChatMessages(data.data.id).catch(console.error);
                // Load chat history with game data
                loadChatHistory(data.data.id, data.data).catch(console.error);
            }
        })
    };

    const startGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('start', {
            game_id: gameId,
        })
    };

    const addBot = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('add-bot', {
            game_id: gameId,
        })
    };

    const exitGame = (gameId: string, botId?: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('exit', {
            game_id: gameId,
            bot_id: botId,
        }, {
            onSuccess: (data) => {
                // If user removed themselves (not a bot), mark as spectating and switch channels
                if (!botId) {
                    setSpectatorGames(prev => new Set(prev).add(gameId));
                    
                    // Clean up old game-user channel and switch to game channel for spectators
                    const oldChannelName = `gu-${gameId}-${user_id}`;
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
    };

    // Hmm loading the url should add the player to the game.

    const loadGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('status', {
            game_id: gameId,
        }, {
            onSuccess: (data) => {
                setGameId(data.data.id);
                joinOrSubscribe(data.data);
            }
        })
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
            // game self + waiting -> subscribe to gu
            // game self + not waiting -> subscribe to gu
            subscribeToGame(gameId).catch(console.error);
            subscribeToChatMessages(gameId).catch(console.error);
            return;
        }
        
        // Check if user is intentionally spectating this game
        const isSpectating = spectatorGames.has(gameId);
        
        // no game self + waiting + not spectating + room available -> join
        // no game self + (not waiting OR spectating OR no room) -> subscribe to game
        if (!isSpectating && game.status === 'waiting' && game.players.length < MAX_PLAYERS) {
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

    const attack = (cards: Card[]): Promise<{ game_id: string }> => {
        // Optimistic game state update after animation completes
        setTimeout(() => {
            const g: PersonalGame = games[game_id!];
            if (!g) return;
            
            const table_battles = g.table_battles;
            const newHand = g.self.hand.filter(card => !cards.some(c => card_comp(c, card)));
            
            setGames(prev => ({ 
                ...prev, 
                [game_id!]: { 
                    ...prev[game_id!], 
                    table_battles: [...table_battles, ...cards.map(card => ({ attack: card, defense: null }))], 
                    self: { ...prev[game_id!].self, hand: newHand } 
                } 
            }));
            
            // Update local hand order
            setLocalHandOrders(prev => ({
                ...prev,
                [game_id!]: (prev[game_id!] || []).filter(card => !cards.some(c => c.suit === card.suit && c.value === card.value))
            }));
            
            console.log('[OPTIMISTIC] Updated game state after attack animation');
        }, ANIMATION_TIME);

        // Server API call
        return invokeGameFunctions('attack', {
            game_id: game_id!,
            cards: cards,
        });
    };

    const pass = (cards: Card[]): Promise<{ game_id: string }> => {
        // Optimistic game state update after animation completes
        setTimeout(() => {
            const g: PersonalGame = games[game_id!];
            if (!g) return;
            
            const table_battles = g.table_battles;
            const next_defender = get_next_player_index(g, g.defender);
            const newHand = g.self.hand.filter(card => !cards.some(c => card_comp(c, card)));
            
            setGames(prev => ({ 
                ...prev, 
                [game_id!]: { 
                    ...prev[game_id!], 
                    table_battles: [...table_battles, ...cards.map(card => ({ attack: card, defense: null }))], 
                    self: { ...prev[game_id!].self, hand: newHand }, 
                    defender: next_defender 
                } 
            }));
            
            // Update local hand order
            setLocalHandOrders(prev => ({
                ...prev,
                [game_id!]: (prev[game_id!] || []).filter(card => !cards.some(c => c.suit === card.suit && c.value === card.value))
            }));
            
            console.log('[OPTIMISTIC] Updated game state after pass animation');
        }, ANIMATION_TIME);

        // Server API call
        return invokeGameFunctions('pass', {
            game_id: game_id!,
            cards: cards,
        });
    };

    const pickup = (): Promise<{ game_id: string }> => {
        // Optimistic game state update after animation completes
        setTimeout(() => {
            const g: PersonalGame = games[game_id!];
            if (!g) return;
            
            const table_battles = g.table_battles;
            const next_first_attacker = get_next_player_index(g, g.defender);
            const next_defender = get_next_player_index(g, next_first_attacker);
            
            // Collect all cards from the table (both attacks and defenses)
            const allTableCards = table_battles.flatMap(battle => 
                battle.defense ? [battle.attack, battle.defense] : [battle.attack]
            );
            const newHand = [...g.self.hand, ...allTableCards];
            
            setGames(prev => ({
                ...prev,
                [game_id!]: {
                    ...prev[game_id!],
                    table_battles: [],
                    self: {
                        ...prev[game_id!].self,
                        hand: newHand
                    },
                    first_attacker: next_first_attacker,
                    defender: next_defender
                }
            }));
            
            // Update local hand order (add new cards to the end)
            setLocalHandOrders(prev => ({
                ...prev,
                [game_id!]: [...(prev[game_id!] || []), ...allTableCards]
            }));
            
            console.log('[OPTIMISTIC] Updated game state after pickup animation');
        }, ANIMATION_TIME);

        // Server API call
        return invokeGameFunctions('pickup', {
            game_id: game_id!,
        });
    };

    const cover = (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        // Optimistic game state update after animation completes
        setTimeout(() => {
            const g: PersonalGame = games[game_id!];
            if (!g) return;
            
            const newHand = g.self.hand.filter(card => !coverCards.some(c => card_comp(c, card)));
            const updatedTableBattles = g.table_battles.map(battle => {
                const attackIndex = attackCards.findIndex(card => 
                    card_comp(card, battle.attack)
                );
                if (attackIndex !== -1) {
                    return { ...battle, defense: coverCards[attackIndex] };
                }
                return battle;
            });
            
            setGames(prev => ({
                ...prev, 
                [game_id!]: {
                    ...prev[game_id!],
                    table_battles: updatedTableBattles,
                    self: { ...prev[game_id!].self, hand: newHand }
                }
            }));
            
            // Update local hand order
            setLocalHandOrders(prev => ({
                ...prev,
                [game_id!]: (prev[game_id!] || []).filter(card => !coverCards.some(c => card_comp(c, card)))
            }));
            
            console.log('[OPTIMISTIC] Updated game state after cover animations');
        }, ANIMATION_TIME);

        // Server API call
        return invokeGameFunctions('cover', {
            game_id: game_id!,
            cover_cards: coverCards,
            attack_cards: attackCards,
        });
    };

    const good = (): Promise<{ game_id: string }> => {
        return invokeGameFunctions('good', {
            game_id: game_id!,
        });
    };

    const sendMessage = async (message: string): Promise<void> => {
        if (!game_id || !user_id) {
            throw new Error('No game or user available');
        }

        if (!message || message.trim() === '') {
            throw new Error('Message cannot be empty');
        }

        if (message.length > 1000) {
            throw new Error('Message is too long');
        }

        const trimmedMessage = message.trim();

        // Save message to database - the trigger will handle broadcasting
        const { error } = await supabase
            .from('chat_messages')
            .insert({
                game_id: game_id,
                user_id: user_id,
                message: trimmedMessage,
                is_system: false
            });

        if (error) {
            throw error;
        }
    };

    const updateGameName = (gameId: string, name: string): Promise<{ game_id: string }> => {
        const previousName = games[gameId]?.name;

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

        return invokeGameFunctions('update-name', {
            game_id: gameId,
            new_name: name
        }, {
            onError: revert
        });
    };

    const rearrangePlayer = (gameId: string, playerIndices: number[]): Promise<{ game_id: string }> => {
        const previousPlayers = games[gameId]?.players ? [...games[gameId].players] : [];
        if (previousPlayers.length === 0) {
            return Promise.reject(new Error(`Cannot rearrange players`));
        }
        const rearrangedPlayers = playerIndices.map(index => previousPlayers[index]);
        setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], players: rearrangedPlayers } }));

        const revert = () => {
            if (previousPlayers.length === 0) {
                return;
            }
            setGames(prev => ({ ...prev, [gameId]: { ...prev[gameId], players: previousPlayers } }));
        }

        return invokeGameFunctions('rearrange-players', {
            game_id: gameId,
            player_indices: playerIndices
        }, {
            onError: revert
        });
    };

    const rearrangeHand = (gameId: string, cardIndices: number[]): Promise<{ game_id: string }> => {
        const previousHand = games[gameId]?.self?.hand ? [...games[gameId].self.hand] : [];

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

        return invokeGameFunctions('rearrange-hand', {
            game_id: gameId,
            card_indices: cardIndices
        }, {
            onError: revert
        });
    };

    const getUserGames = async (): Promise<void> => {
        // This needs to also throw in status at least
        try {
            if (!user_id) {
                console.log('No player_id available for getUserGames');
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

                // Fuck you I know this will be a public game type
                const game = playerHand.games as unknown as PublicGame;
                const selfPlayer = game.players.find((player) => player.player_id === user_id);
                if (!selfPlayer){
                    continue
                }
                games[game.id] = {
                    ...game,
                    self: {...selfPlayer, player_id: user_id, hand: playerHand.hand, awaiting_attack: playerHand.awaiting_attack, done_attacking_this_round: false}// as unknown as PrivatePlayer
                };

            }

            setGames(prev => ({ ...prev, ...games }));

        } catch (error) {
            console.error('Error in getUserGames:', error);
        }
    };

    const continueGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('continue', { game_id: gameId });
    };

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
            const game_id = data.data.id;

            setGames(prev => ({
                ...prev,
                [game_id]: mergeGameData(game_id, data.data, prev)
            }))

            options.onSuccess?.(data as T);

            return { game_id };

        } catch (error) {
            options.onError?.(error);
            throw error;
        }
    }

    const currentChatMessages = chatMessages[game_id!] || [];
    const currentLocalHandOrder = localHandOrders[game_id!] || [];

    return (
        <ServerContext.Provider value={{
            createGame,
            joinGame,
            startGame,
            addBot,
            exitGame,
            game_id,
            game: games[game_id!],
            games,
            attack,
            pass,
            pickup,
            cover,
            good,
            sendMessage,
            getUserGames,
            updateGameState: (gameId: string, gameState: any) => {
                console.log('[SERVER] Updating intermediate game state for game', gameId);
                setGames(prev => ({ 
                    ...prev, 
                    [gameId]: mergeGameData(gameId, gameState, prev) 
                }));
            },
            updateGameName,
            rearrangePlayer,
            rearrangeHand,
            continueGame,
            gameLoadError,
            chatMessages: currentChatMessages,
            localHandOrder: currentLocalHandOrder,
            setLocalHandOrder: (order: Card[]) => {
                if (game_id) {
                    setLocalHandOrders(prev => ({ ...prev, [game_id]: order }));
                }
            }
        }}>
            {children}
        </ServerContext.Provider>
    );
};

interface ServerContextType {
    createGame: () => Promise<{ game_id: string }>;
    joinGame: (gameId: string) => Promise<{ game_id: string }>;
    startGame: (gameId: string) => Promise<{ game_id: string }>;
    addBot: (gameId: string) => Promise<{ game_id: string }>;
    exitGame: (gameId: string, botId?: string) => Promise<{ game_id: string }>;
    game_id: string | null;
    game: PersonalGame | null;
    games: { [key: string]: PersonalGame };
    attack: (cards: Card[]) => Promise<{ game_id: string }>;
    pass: (cards: Card[]) => Promise<{ game_id: string }>;
    pickup: () => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[]) => Promise<{ game_id: string }>;
    good: () => Promise<{ game_id: string }>;
    sendMessage: (message: string) => Promise<void>;
    getUserGames: () => Promise<void>;
    updateGameState: (gameId: string, gameState: any) => void;
    updateGameName: (gameId: string, name: string) => Promise<{ game_id: string }>;
    rearrangePlayer: (gameId: string, playerIndices: number[]) => Promise<{ game_id: string }>;
    rearrangeHand: (gameId: string, cardIndices: number[]) => Promise<{ game_id: string }>;
    continueGame: (gameId: string) => Promise<{ game_id: string }>;
    gameLoadError: string | null;
    chatMessages: any[];
    localHandOrder: Card[];
    setLocalHandOrder: (order: Card[]) => void;
}

export const useServer = () => {
    const context = useContext(ServerContext);
    if (!context) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    return context;
}; 
