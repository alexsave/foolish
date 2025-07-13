import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, PublicPlayer, PersonalGame, PublicGame, SERVER_EVENT_TYPE } from '../common/types';
import supabase from '../backend/Connector';
import { useParams } from 'react-router-dom';
import { useAuth } from './AuthContext';
import { MAX_PLAYERS } from '../common/constants';
import { get_next_player_index, canCover } from '../common/common_utils';

const ServerContext = createContext<ServerContextType | null>(null);

const handsQuery = 
`game_id,
hand,
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
    updated_at
)`;

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {

    const { user_id } = useAuth()

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
    const [localHandOrders, setLocalHandOrders] = useState<{ [key: string]: Card[] }>({});

    // Use ref to avoid closure issues in WebSocket handler
    const gameIdRef = useRef<string | null>(null);
    const userNamesRef = useRef<{ [userId: string]: string }>({});

    // Use ref to prevent duplicate user effect executions
    const prevUserRef = useRef<string | null>(null);

    useEffect(() => {
        //const game_id = use
        if (url_game_id) {
            gameIdRef.current = url_game_id;
            setGameId(url_game_id);
            setGameLoadError(null); // Clear any previous errors
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
            return;
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
        // Ensure we have proper auth before subscribing
        await supabase.realtime.setAuth();

        // Subscribe to personalized game-user channel for game updates
        const gameUserChannel = supabase.channel(`gu-${gameId}-${user_id}`, {
            config: { private: true }
        });

        gameUserChannel
            .on('broadcast', { event: 'game_update' }, (payload) => {
                console.log('Game update received:', payload);
                handleGameMessage(payload.payload);
            })
            .subscribe((status, err) => {
                if (status === 'SUBSCRIBED') {
                    console.log('Connected to game-user channel:', `gu-${gameId}-${user_id}`);
                } else {
                    console.error('Game-user channel error:', err);
                }
            });
    };

    const subscribeToChatMessages = async (gameId: string) => {
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
                    console.log('Connected to chat channel:', `chat:${gameId}`);
                } else {
                    console.error('Chat channel error:', err);
                }
            });
    };

    const handleGameMessage = (message: any) => {
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

        // Handle animation events if present
        if (actualMessage.animation_events && actualMessage.animation_events.length > 0) {
            console.log('Animation events received:', actualMessage.animation_events);
            // Trigger custom event for animation context to listen to
            const animationEvent = new CustomEvent('gameAnimationEvents', {
                detail: { events: actualMessage.animation_events, gameId: messageGameId }
            });
            window.dispatchEvent(animationEvent);
        }

        // Handle all the different message types using the extracted message
        const gameData = actualMessage.game || message.game;

        if (actualMessage.type === SERVER_EVENT_TYPE.PLAYER_JOINED_GAME) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PLAYER_LEFT_GAME) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PLAYER_READY) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.GAME_STARTED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.FLIPPED_CARD) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.FIRST_ATTACKER) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.ATTACK_PLAYED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PASS_PLAYED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PICKUP_PLAYED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.COVER_PLAYED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PLAYER_WON) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PLAYABLE_CARDS) {
            // This is a personal message - update game state and show notification
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
            console.log('Playable cards message:', actualMessage.message);
        } else if (actualMessage.type === 'no_more_attacks') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'free_play_mode') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'game_done') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'deck_ran_out') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'player_refilled') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'player_wins') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === 'game_created') {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.GAME_NAME_UPDATED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.PLAYERS_REARRANGED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else if (actualMessage.type === SERVER_EVENT_TYPE.HAND_REARRANGED) {
            setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
        } else {
            // Default handler for other message types
            if (gameData) {
                setGames(prev => ({ ...prev, [messageGameId]: mergeGameData(messageGameId, gameData, prev) }));
            }
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
                setGameId(data.data.game.id);
                setGames(prev => ({ ...prev, [data.data.game.id]: mergeGameData(data.data.game.id, data.data.game, prev) }));
                // Subscribe to the new game's channel and chat messages
                subscribeToGame(data.data.game.id).catch(console.error);
                subscribeToChatMessages(data.data.game.id).catch(console.error);
            }
        });
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('join', {
            game_id: gameId,
        }, {
            onSuccess: (data) => {
                setGameId(data.data.game.id);
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
                
                //setGames(prev => ({ ...prev, [data.data.game.id]: mergeGameData(data.data.game.id, data.data.game, prev) }));
                // Subscribe to the game's channel and chat messages
                subscribeToGame(data.data.game.id).catch(console.error);
                subscribeToChatMessages(data.data.game.id).catch(console.error);
                // Load chat history with game data
                loadChatHistory(data.data.game.id, data.data.game).catch(console.error);
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
                        gameChannel.on('broadcast', { event: 'game_update' }, (payload) => {
                            console.log('Game update received:', payload);
                            handleGameMessage(payload.payload);
                        });
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
                setGameId(data.data.game.id);
                joinOrSubscribe(data.data.game);
            }
        })
    };

    const joinOrSubscribe = (game: PersonalGame) => {
        const gameId = game.id;
        
        // Set game_id state and game data first, then load chat history
        setGameId(gameId);
        setGames(prev => ({ ...prev, [gameId]: mergeGameData(gameId, game, prev) }));
        
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
                gameChannel.on('broadcast', { event: 'game_update' }, (payload) => {
                    console.log('Game update received:', payload);
                    handleGameMessage(payload.payload);
                });
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
        // Quick check to see if valid attack, then optimistic update
        // What is most likely to happen?
        // let's assume that they are using this client, so they attacked with a hand in their deck.
        // All we need to confirm is that the valu is on the board + defender has enough

        const g: PersonalGame = games[game_id!];
        const table_battles = g.table_battles;
        let uncovered_cards = table_battles.filter(battle => battle.defense === null).length;

        const defender: PublicPlayer = g.players[g.defender];

        let defender_cards = defender.hand_length;

        if (uncovered_cards + cards.length > defender_cards) {
            return Promise.reject(new Error(`No room in defenders hand`));
        }
        if (table_battles.length > 0 && !cards.every(card => table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            return Promise.reject(new Error(`Some card values are not on the table`));
        }

        // Optimistic update
        const newHand = g.self.hand.filter(card => !cards.includes(card));
        setGames(prev => ({ ...prev, [game_id!]: { ...prev[game_id!], table_battles: [...table_battles, ...cards.map(card => ({ attack: card, defense: null }))], self: { ...prev[game_id!].self, hand: newHand } } }));
        
        // Update local hand order
        setLocalHandOrders(prev => ({
            ...prev,
            [game_id!]: (prev[game_id!] || []).filter(card => !cards.some(c => c.suit === card.suit && c.value === card.value))
        }));

        return invokeGameFunctions('attack', {
            game_id: game_id!,
            cards: cards,
        });
    };

    const pass = (cards: Card[]): Promise<{ game_id: string }> => {
        // Quick check to see if valid attack, then optimistic update
        // We're also leaning on checks elsewhere in the client
        const g: PersonalGame = games[game_id!];
        const table_battles = g.table_battles;
        if (!cards.every(card => card.value === cards[0].value)) {
            return Promise.reject(new Error(`Some card values are not the same`));
        }
        if (!table_battles.every(battle => battle.defense === null && battle.attack.value === cards[0].value)) {
            return Promise.reject(new Error(`Cannot pass`));
        }

        // Optimistic update
        // I don't like this cast 
        // But refactoring it to work would involve also changing the player type
        const next_defender = get_next_player_index(g, g.defender);
        const newHand = g.self.hand.filter(card => !cards.includes(card));
        setGames(prev => ({ ...prev, [game_id!]: { ...prev[game_id!], table_battles: [...table_battles, ...cards.map(card => ({ attack: card, defense: null }))], self: { ...prev[game_id!].self, hand: newHand }, defender: next_defender } }));
        
        // Update local hand order
        setLocalHandOrders(prev => ({
            ...prev,
            [game_id!]: (prev[game_id!] || []).filter(card => !cards.some(c => c.suit === card.suit && c.value === card.value))
        }));

        return invokeGameFunctions('pass', {
            game_id: game_id!,
            cards: cards,
        });
    };

    const pickup = (): Promise<{ game_id: string }> => {
        // Optimistic update
        const g: PersonalGame = games[game_id!];
        const table_battles = g.table_battles;
        if (table_battles.length === 0) {
            return Promise.reject(new Error(`Cannot pickup`));
        }
        const next_first_attacker = get_next_player_index(g, g.defender);
        const next_defender = get_next_player_index(g, next_first_attacker);
        // move all table cards to self, defenses and attacks
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

        return invokeGameFunctions('pickup', {
            game_id: game_id!,
        });
    };

    const cover = (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        // Optimistic update
        const g: PersonalGame = games[game_id!];
        const table_battles = g.table_battles;
        if (table_battles.length === 0) {
            return Promise.reject(new Error(`Cannot cover`));
        }
        for (let i = 0; i < coverCards.length; i++) {
            const coverCard = coverCards[i];
            const attackCard = attackCards[i];
            if (!canCover(attackCard, coverCard, g.power_suit)) {
                return Promise.reject(new Error(`Cover card value does not match attack card value`));
            }
        }

        // Optimistic update. Move cover cards out of self, put cover card as defense on corresponding attack
        const newHand = g.self.hand.filter(card => !coverCards.includes(card));
        setGames(prev => ({
            ...prev, [game_id!]: {
                ...prev[game_id!],

                // this could use card_comp
                table_battles: table_battles.map(battle => attackCards.findIndex(card => card.value === battle.attack.value && card.suit === battle.attack.suit) !== -1 ? { ...battle, defense: coverCards[attackCards.findIndex(card => card.value === battle.attack.value && card.suit === battle.attack.suit)] } : battle),

                self: { ...prev[game_id!].self, hand: newHand }
            }
        }));
        
        // Update local hand order
        setLocalHandOrders(prev => ({
            ...prev,
            [game_id!]: (prev[game_id!] || []).filter(card => !coverCards.some(c => c.suit === card.suit && c.value === card.value))
        }));
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
            name: name
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
                games[game.id] = {
                    ...game,
                    self: playerHand.hand// as unknown as PrivatePlayer
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
            const game_id = data.data.game.id;

            setGames(prev => ({
                ...prev,
                [game_id]: mergeGameData(game_id, data.data.game, prev)
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