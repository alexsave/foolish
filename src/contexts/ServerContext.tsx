import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, Game, GAME_MOVE_TYPE, LOBBY_MOVE_TYPE, LobbyGame, PersonalGame, SERVER_EVENT_TYPE } from '../common/types';
import supabase from '../backend/Connector';
import { useParams } from 'react-router-dom';
import { useAuth } from './AuthContext';
import { emailToName } from '../common/utils';

const ServerContext = createContext<ServerContextType|null>(null);

const HOST = '10.0.0.243';

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {

    const {user} = useAuth()

    const url_game_id = useParams().game_id;
    // keep a state of games
    // maybe ref idk
    const [games, setGames] = useState<{[key: string]: (Game | LobbyGame | PersonalGame)}>({});

    //const [user, setUser] = useState(null);
    const [loading, setLoading] = useState(true);


    const [game_id, setGameId] = useState<string | null>(null);
    // get game id from url
    //const [game_id, setGameId] = useState<string | null>(null);

    // very important for making requests
    const [player_id, setPlayerId] = useState<string | null>(null);

    //useref websocket for sure
    const webSocketRef = useRef<WebSocket | null>(null);
    
    // Use ref to avoid closure issues in WebSocket handler
    const gameIdRef = useRef<string | null>(null);

    useEffect(() => {
        //const game_id = use
        console.log('url game id', url_game_id);
        if (url_game_id) {
            setGameId(url_game_id);
            if (!games[url_game_id]) {
                console.log('info not in games, loading game');
                loadGame(url_game_id);
            } else {
                console.log('info in games, not loading game');
            }
        }
    }, [url_game_id]);


    // Keep ref in sync with state
    useEffect(() => {
        gameIdRef.current = game_id;
        if (game_id && player_id) {
            console.log('game id changed, need to fetch game data');
            // fetch game data. for now it will just be lobby info
            loadGame(game_id);

        }
    }, [game_id]);


    useEffect(() => {
        if (user) {
            setPlayerId(user);
            setupRealtimeSubscriptions().catch(console.error);
        }

        // cleanup realtime subscriptions
        return () => {
            if (webSocketRef.current) {
                webSocketRef.current.close();
            }
            // Remove all realtime subscriptions
            supabase.removeAllChannels();
        };
    }, []);

    // Setup Supabase Realtime subscriptions
    const setupRealtimeSubscriptions = async () => {
        // Get current session and set auth token for realtime
        await supabase.realtime.setAuth();
        
        // Subscribe to private user channel for personal messages
        const privateChannel = supabase.realtime.channel(`user-${user}`, {
            config: { private: true }
        });
        
        privateChannel
            .on('broadcast', { event: 'private_message' }, (payload) => {
                console.log('Private message received:', payload);
                // Handle private messages (like hand updates, personal notifications)
                handlePrivateMessage(payload.payload);
            })
            .subscribe((status, err) => {
                if (status === 'SUBSCRIBED') {
                    console.log('Connected to private channel:', `user-${user}`);
                } else {
                    console.error('Private channel error:', err, status);
                }
            });

        // We'll subscribe to game channels when we join/create games
        console.log('Realtime subscriptions set up for user:', user);
    };

    const subscribeToGame = async (gameId: string) => {
        // Ensure we have proper auth before subscribing
        await supabase.realtime.setAuth();
        
        // Subscribe to public game channel for game updates
        const gameChannel = supabase.channel(`game-${gameId}`, {
            config: { private: true }
        });
        
        gameChannel
            .on('broadcast', { event: 'game_message' }, (payload) => {
                console.log('Game message received:', payload);
                handleGameMessage(payload.payload);
            })
            .subscribe((status, err) => {
                if (status === 'SUBSCRIBED') {
                    console.log('Connected to game channel:', gameId);
                } else {
                    console.error('Game channel error:', err);
                }
            });
    };

    const handleGameMessage = (message: any) => {
        // Process game messages similar to WebSocket messages
        if (!gameIdRef.current || message.game_id !== gameIdRef.current) {
            return;
        }

        if (message.type === SERVER_EVENT_TYPE.PLAYER_JOINED_GAME) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else if (message.type === SERVER_EVENT_TYPE.PLAYER_READY) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else if (message.type === SERVER_EVENT_TYPE.GAME_STARTED) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else if (message.type === SERVER_EVENT_TYPE.ATTACK_PLAYED) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else if (message.type === SERVER_EVENT_TYPE.PASS_PLAYED) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else if (message.type === SERVER_EVENT_TYPE.PICKUP_PLAYED) {
            setGames(prev => ({...prev, [message.game_id]: message.game}));
        } else {
            // Default handler for other message types
                // else if chat message, it's a bit different. chat will be stored separeate
            if (message.game) {
                setGames(prev => ({...prev, [message.game_id]: message.game}));
            }
        }
    };

    const handlePrivateMessage = (message: any) => {
        // Handle private messages like hand updates, personal notifications
        console.log('Processing private message:', message);
        // Implement specific private message handling as needed
        handleGameMessage(message);
    };

    const promiseMaker = (endpoint: string, body: any, dataHandler: (data: any) => void): Promise<any> => {
        return new Promise((resolve, reject) => {
            fetch(`http://${HOST}:3009/${endpoint}`, {
                method: 'POST',
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(body)
            }).then(response => {
                if (!response.ok) {
                    throw new Error(`HTTP error! status: ${response.status}`);
                }
                return response.json();
            }).then(data => {
                dataHandler(data);
                resolve(data);
            }).catch(error => {
                console.error('Promise maker error', error);
                reject(error);
            });
        });
    }



    const createGame = (): Promise<{ game_id: string }> => {
        const promise = new Promise<{game_id: string}>((resolve, reject) => {
            supabase.functions.invoke('create', {
                body: {}
            }).then(data => {
                resolve({game_id: data.data.game.id});  
                setGameId(data.data.game.id);
                setGames(prev => ({...prev, [data.data.game.id]: data.data.game}));
                // Subscribe to the new game's channel
                subscribeToGame(data.data.game.id).catch(console.error);
            }).catch(error => {
                reject(error);
            });
        });
        return promise;
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        const promise = new Promise<{game_id: string}>((resolve, reject) => {
            supabase.functions.invoke('join', {
                body: {
                    game_id: gameId,
                }
            }).then(data => {
                resolve({game_id: data.data.game.id});  
                setGameId(data.data.game.id);
                setGames(prev => ({...prev, [data.data.game.id]: data.data.game}));
                // Subscribe to the game's channel
                subscribeToGame(data.data.game.id).catch(console.error);
            }).catch(error => {
                reject(error);
            });
        });
        return promise;
    };

    const startGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.START, { game_id: gameId, player_id: player_id }, (data) => {
            setGameId(data.game_id);
            // WS will also do this, but it might have a delay
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const loadGame = (gameId: string): Promise<{ game_id: string }> => {
        const promise = new Promise<{game_id: string}>((resolve, reject) => {
            supabase.functions.invoke('status', {
                body: {
                    game_id: gameId,
                }
            }).then(data => {
                resolve({game_id: data.data.game.id});  
                setGameId(data.data.game.id);
                setGames(prev => ({...prev, [data.data.game.id]: data.data.game}));
                // Subscribe to the game's channel
                subscribeToGame(data.data.game.id).catch(console.error);
            }).catch(error => {
                reject(error);
            });
        });
        return promise;
    };

    const attack = (cards: Card[]): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.ATTACK, { game_id: game_id!, player_id: player_id!, cards: cards }, (data) => {
            // clear selected cards
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const pass = (cards: Card[]): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.PASS, { game_id: game_id!, player_id: player_id!, cards: cards }, (data) => {
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const pickup = (): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.PICKUP, { game_id: game_id!, player_id: player_id! }, (data) => {
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const cover = (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.COVER, { game_id: game_id!, player_id: player_id!, cover_cards: coverCards, attack_cards: attackCards }, (data) => {
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const good = (): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.GOOD, { game_id: game_id!, player_id: player_id! }, (data) => {
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const setGameIdFromUrl = (gameId: string) => {
        if (gameId !== game_id) {
            setGameId(gameId);
        }
        if (player_id && game_id) {
            loadGame(game_id);
        }
    };

    return (
        <ServerContext.Provider value={{
            createGame,
            joinGame,
            startGame,
            game_id,
            game: games[game_id!],
            player_id,
            loadGame,   
            attack,
            pass,
            pickup,
            cover,
            good,
            setGameIdFromUrl
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
    game: Game | LobbyGame | PersonalGame | null;
    player_id: string | null;
    loadGame: (gameId: string) => Promise<{ game_id: string }>;
    attack: (cards: Card[]) => Promise<{ game_id: string }>;
    pass: (cards: Card[]) => Promise<{ game_id: string }>;
    pickup: () => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[]) => Promise<{ game_id: string }>;
    setGameIdFromUrl: (gameId: string) => void;
    good: () => Promise<{ game_id: string }>;
}

export const useServer = () => {
    const context = useContext(ServerContext);
    if (!context) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    return context;
}; 