import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { LOBBY_MOVE_TYPE } from '../common/common';
//import supabase from '../db/supabaseClient';
import { useParams } from 'react-router-dom';

const ServerContext = createContext<ServerContextType|null>(null);


// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const [user, setUser] = useState(null);
    const [loading, setLoading] = useState(true);


    const [game_id, setGameId] = useState<string | null>(null);
    // get game id from url
    //const [game_id, setGameId] = useState<string | null>(null);

    // very important for making requests
    const [player_id, setPlayerId] = useState<string | null>(null);

    //useref websocket for sure
    const webSocketRef = useRef<WebSocket | null>(null);

    const url_game_id = useParams().game_id;
    useEffect(() => {
        // keep it in sync with url
        console.log('url game id changed ' + url_game_id + ' game id is ' + game_id);
        if (url_game_id && url_game_id !== game_id) {
            setGameId(url_game_id);
        }
    }, [url_game_id]);

    useEffect(() => {
        if (game_id) {
            console.log('game id changed, need to fetch game data');
            // fetch game data. for now it will just be lobby info

        }
    }, [game_id]);

    useEffect(() => {

        // this is cleanup, right?
        return () => {
            if (webSocketRef.current) {
                webSocketRef.current.close();
            };
        };
    }, []);

    // pass as param just to be safe with useState
    const createWebSocket = (pId: string) => {
        const ws = new WebSocket('ws://localhost:3001');
        ws.onmessage = (event) => {
            const data = JSON.parse(event.data);
            console.log(data);
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
            fetch('http://localhost:3009/' + endpoint, {
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
        return promiseMaker(LOBBY_MOVE_TYPE.CREATE, { player_id: player_id }, (data) => {
            setGameId(data.game_id);
        });
    };

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.JOIN, { game_id: gameId, player_id: player_id }, (data) => {
            setGameId(data.game_id);
        });
    };

    const startGame = (gameId: string): Promise<{ game_id: string }> => {
        return promiseMaker(LOBBY_MOVE_TYPE.START, { game_id: gameId, player_id: player_id }, (data) => {
            setGameId(data.game_id);
        });
    };

    return (
        <ServerContext.Provider value={{
            serverLogin,
            createGame,
            joinGame,
            startGame,
            game_id
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
}

export const useServer = () => {
    const context = useContext(ServerContext);
    if (!context) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    return context;
}; 