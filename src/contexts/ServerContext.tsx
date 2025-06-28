import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, Game, GAME_MOVE_TYPE, LOBBY_MOVE_TYPE, LobbyGame, PersonalGame, SERVER_EVENT_TYPE } from '../common/types';
//import supabase from '../db/supabaseClient';

const ServerContext = createContext<ServerContextType|null>(null);

const HOST = '10.0.0.243';

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {

    // keep a state of games
    // maybe ref idk
    const [games, setGames] = useState<{[key: string]: (Game | LobbyGame | PersonalGame)}>({});

    const [user, setUser] = useState(null);
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
        // write to player id to local storage
        if (player_id) {
            localStorage.setItem('player_id', player_id);
        }
    }, [player_id]);

    useEffect(() => {
        // read from local storage for player id
        const playerId = localStorage.getItem('player_id');
        //console.log('player id is ' + playerId);
        if (playerId) {
            setPlayerId(playerId);
            createWebSocket(playerId);
        }

        // this is cleanup, right?
        return () => {
            if (webSocketRef.current) {
                webSocketRef.current.close();
            };
        };
    }, []);

    // pass as param just to be safe with useState
    const createWebSocket = (pId: string) => {
        const ws = new WebSocket(`ws://${HOST}:3001`);
        ws.onmessage = (event) => {
            const data = JSON.parse(event.data);
            console.log(data);

            // Ok so this has SOME important stuff
            // Let's start with server lobby events
            // first assert that gameid = current game id, otherwise dont worry parsing it
            if (gameIdRef.current && data.game_id !== gameIdRef.current) {
                return;
            }

            const message = data.message;
            if (message.type === SERVER_EVENT_TYPE.PLAYER_JOINED_GAME) {
                // Because a list of names + statuses of max length 8 isn't THAT long, we'll just send over the entire game
                setGames({...games, [data.game_id]: message.game});
            } else if (message.type === SERVER_EVENT_TYPE.PLAYER_READY) {
                setGames({...games, [data.game_id]: message.game});
            } else if (message.type === SERVER_EVENT_TYPE.GAME_STARTED) {
                setGames({...games, [data.game_id]: message.game});
            } else if (message.type === SERVER_EVENT_TYPE.ATTACK_PLAYED) {
                setGames({...games, [data.game_id]: message.game});
            } else if (message.type === SERVER_EVENT_TYPE.PASS_PLAYED) {
                setGames({...games, [data.game_id]: message.game});
            } else if (message.type === SERVER_EVENT_TYPE.PICKUP_PLAYED) {
                setGames({...games, [data.game_id]: message.game});
            } else {
                // honestly just do this
                setGames({...games, [data.game_id]: message.game});
            }


        };
        ws.onopen = () => {
            console.log('Connected to server');

            // Send a login to associate the player id with the websocket
            ws.send(JSON.stringify({
                type: LOBBY_MOVE_TYPE.WEBSOCKET_CONNECT,
                player_id: pId
            }));

        };
        ws.onclose = () => {
            console.log('Disconnected from server');
        };
        ws.onerror = (event) => {
            console.log('Error', event);
        };
        webSocketRef.current = ws;
    }

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

    const serverLogin = (name: string): Promise<{ name: string, player_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.LOGIN, { name: name }, (data) => {
            setPlayerId(data.player_id);
            createWebSocket(data.player_id);
        });
    };

    const createGame = (): Promise<{ game_id: string }> => {
        // this should not load the game data yet
        return promiseMaker(LOBBY_MOVE_TYPE.CREATE, { player_id: player_id }, (data) => {
            setGameId(data.game_id);
            // The server should also return game data for this game
            // so we can set the games state
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.JOIN, { game_id: gameId, player_id: player_id }, (data) => {
            setGameId(data.game_id);
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const startGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.START, { game_id: gameId, player_id: player_id }, (data) => {
            setGameId(data.game_id);
            // WS will also do this, but it might have a delay
            setGames({...games, [data.game_id]: data.game});
        });
    };

    const loadGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(GAME_MOVE_TYPE.STATUS, { game_id: gameId, player_id: player_id }, (data) => {
            setGames({...games, [data.game_id]: data.game});
        });
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
            serverLogin,
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
    serverLogin: (name: string) => Promise<{name: string, player_id: string}>;
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