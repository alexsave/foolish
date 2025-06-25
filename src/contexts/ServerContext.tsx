import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { LOBBY_MOVE_TYPE } from '../common/common';
//import supabase from '../db/supabaseClient';

const ServerContext = createContext<ServerContextType|null>(null);

interface ServerContextType {
    serverLogin: (name: string) => Promise<{name: string, player_id: string}>;
    createGame: () => Promise<{ game_id: string }>;
    joinGame: (gameId: string) => Promise<{ game_id: string }>;
    game_id: string | null;
}

// for now we'll just use a fake auth impl
// this will be kinda similar to client.js
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const [user, setUser] = useState(null);
    const [loading, setLoading] = useState(true);

    const [game_id, setGameId] = useState<string | null>(null);

    // very important for making requests
    const [player_id, setPlayerId] = useState<string | null>(null);

    //useref websocket for sure
    const webSocketRef = useRef<WebSocket | null>(null);

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

    const serverLogin = (name: string): Promise<{ name: string, player_id: string }> => {
        return new Promise((resolve, reject) => {

            fetch('http://localhost:3009/login', {
                method: 'POST',
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    name: name
                })
            }).then(response => response.json()).then(data => {
                console.log('Login response', data);
                setPlayerId(data.player_id);
                resolve(data);

                // Now that we have a player id, we can start the websocket connection
                createWebSocket(data.player_id);
            }).catch(error => {
                console.error('Login error', error);
                reject(error);
            });
        });
    };

    const createGame = (): Promise<{ game_id: string }> => {
        return new Promise((resolve, reject) => {
            fetch('http://localhost:3009/' + LOBBY_MOVE_TYPE.CREATE, {
                method: 'POST',
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    player_id: player_id
                })
            }).then(response => response.json()).then(data => {
                setGameId(data.game_id);
                resolve(data);
            }).catch(error => {
                console.error('Create game error', error);
                reject(error);
            });
        });
    }

    const joinGame = (gameId: string): Promise<{ game_id: string }> => {
        return new Promise((resolve, reject) => {
            fetch('http://localhost:3009/' + LOBBY_MOVE_TYPE.JOIN, {
                method: 'POST',
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    game_id: gameId,
                    player_id: player_id
                })
            }).then(response => response.json()).then(data => {
                setGameId(data.game_id);
                resolve(data);
            }).catch(error => {
                console.error('Join game error', error);
                reject(error);
            });
        });
    }

    return (
        <ServerContext.Provider value={{
            serverLogin,
            createGame,
            joinGame,
            game_id
        }}>
            {children}
        </ServerContext.Provider>
    );
};

export const useServer = () => {
    const context = useContext(ServerContext);
    if (!context) {
        throw new Error('useServer must be used within a ServerProvider');
    }
    return context;
}; 