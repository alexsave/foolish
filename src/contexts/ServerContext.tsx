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

    const [loading, setLoading] = useState(true);
    const [gameLoadError, setGameLoadError] = useState<string | null>(null);

    const [game_id, setGameId] = useState<string | null>(null);

    // Use ref to avoid closure issues in WebSocket handler
    const gameIdRef = useRef<string | null>(null);

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
                }).then(() => {
                    setLoading(false);
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
            //setupRealtimeSubscriptions().catch(console.error);

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
            console.log("messageGameId", messageGameId);
            console.log("gameIdRef.current", gameIdRef.current);
            console.log("messageGameId !== gameIdRef.current", messageGameId !== gameIdRef.current);
            return;
        }

        // small caveat: because these are broadcast, they won't have personal info. So self should not be overwritten
        const self = games[messageGameId]?.self;
        if (!self) {
            console.log("we have no self info. We either didn't fetch it or it was overwritten")
        }

        // Handle all the different message types using the extracted message
        const gameData = actualMessage.game || message.game;
        console.log(JSON.stringify(gameData) + " gameData");

        if (actualMessage.type === SERVER_EVENT_TYPE.PLAYER_JOINED_GAME) {
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

    // Helper method to merge game data while preserving self when not present in new data
    const mergeGameData = (gameId: string, newGameData: any, prevGames: any) => {
        return {
            ...newGameData,
            self: newGameData.self !== undefined && newGameData.self !== null ? newGameData.self : prevGames[gameId]?.self
        };
    };

    const createGame = (): Promise<{ game_id: string }> => {
        return invokeGameFunctions('create', {}, {
            onSuccess: (data) => {
                setGameId(data.data.game.id);
                setGames(prev => ({ ...prev, [data.data.game.id]: mergeGameData(data.data.game.id, data.data.game, prev) }));
                // Subscribe to the new game's channel
                subscribeToGame(data.data.game.id).catch(console.error);
            }
        });
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('join', {
            game_id: gameId,
        }, {
            onSuccess: (data) => {
                setGameId(data.data.game.id);
                //setGames(prev => ({ ...prev, [data.data.game.id]: mergeGameData(data.data.game.id, data.data.game, prev) }));
                // Subscribe to the game's channel
                subscribeToGame(data.data.game.id).catch(console.error);
            }
        })
    };

    const startGame = (gameId: string): Promise<{ game_id: string }> => {
        return invokeGameFunctions('start', {
            game_id: gameId,
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
        if (game.self) {
            // game self + waiting -> subscribe to gu
            // game self + not waiting -> subscribe to gu
            subscribeToGame(gameId).catch(console.error);
            return;
        }
        // no game self + waiting -> join
        // no game self + not waiting -> subscribe to game
        if (game.status === 'waiting' && game.players.length < MAX_PLAYERS) {
            // This is kinda iffy. It might be best to automatically spectate, but have an option to join,
            console.log('Auto-joining game in waiting status');
            joinGame(gameId).catch(console.error);
        } else {
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
        setGames(prev => ({ ...prev, [game_id!]: { ...prev[game_id!], table_battles: [...table_battles, ...cards.map(card => ({ attack: card, defense: null }))], self: { ...prev[game_id!].self, hand: prev[game_id!].self.hand.filter(card => !cards.includes(card)) } } }));

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
        setGames(prev => ({ ...prev, [game_id!]: { ...prev[game_id!], table_battles: [...table_battles, { attack: cards[0], defense: null }], self: { ...prev[game_id!].self, hand: prev[game_id!].self.hand.filter(card => !cards.includes(card)) }, defender: next_defender } }));

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
        const next_defender = get_next_player_index(g, g.defender);
        // move all table cards to self, defenses and attacks
        setGames(prev => ({ ...prev, [game_id!]: { ...prev[game_id!], table_battles: [], self: { ...prev[game_id!].self, hand: [...prev[game_id!].self.hand, ...table_battles.map(battle => battle.defense ?? battle.attack)] }, defender: next_defender } }));

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
        setGames(prev => ({
            ...prev, [game_id!]: {
                ...prev[game_id!],

                // this could use card_comp
                table_battles: table_battles.map(battle => attackCards.findIndex(card => card.value === battle.attack.value && card.suit === battle.attack.suit) !== -1 ? { ...battle, defense: coverCards[attackCards.findIndex(card => card.value === battle.attack.value && card.suit === battle.attack.suit)] } : battle),

                self: { ...prev[game_id!].self, hand: prev[game_id!].self.hand.filter(card => !coverCards.includes(card)) }
            }
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

    return (
        <ServerContext.Provider value={{
            createGame,
            joinGame,
            startGame,
            game_id,
            game: games[game_id!],
            games,
            //loadGame,
            attack,
            pass,
            pickup,
            cover,
            good,
            //setGameIdFromUrl,
            getUserGames,
            updateGameName,
            rearrangePlayer,
            rearrangeHand,
            gameLoadError
        }}>
            {children}
        </ServerContext.Provider>
    );
};

interface ServerContextType {
    createGame: () => Promise<{ game_id: string }>;
    joinGame: (gameId: string) => Promise<{ game_id: string }>;
    startGame: (gameId: string) => Promise<{ game_id: string }>;
    game_id: string | null;
    game: PersonalGame | null;
    games: { [key: string]: PersonalGame };
    //loadGame: (gameId: string) => Promise<{ game_id: string }>;
    attack: (cards: Card[]) => Promise<{ game_id: string }>;
    pass: (cards: Card[]) => Promise<{ game_id: string }>;
    pickup: () => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[]) => Promise<{ game_id: string }>;
    //setGameIdFromUrl: (gameId: string) => void;
    good: () => Promise<{ game_id: string }>;
    getUserGames: () => Promise<void>;
    updateGameName: (gameId: string, name: string) => Promise<{ game_id: string }>;
    rearrangePlayer: (gameId: string, playerIndices: number[]) => Promise<{ game_id: string }>;
    rearrangeHand: (gameId: string, cardIndices: number[]) => Promise<{ game_id: string }>;
    gameLoadError: string | null;
}

export const useServer = () => {
    const context = useContext(ServerContext);
    if (!context) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    return context;
}; 