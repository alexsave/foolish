
import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { LOBBY_MOVE_TYPE } from '../common/common';
//import supabase from '../db/supabaseClient';

const ServerContext = createContext<ServerContextType|null>(null);

interface ServerContextType {
    serverLogin: (name: string) => Promise<{name: string, player_id: string}>;
    createGame: () => void;
    joinGame: (gameId: string) => void;
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

    const createWebSocket = () => {
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
                player_id: player_id
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
                setPlayerId(data.player_id);
                resolve(data);

                // Now that we have a player id, we can start the websocket connection
                createWebSocket();
            });
        });
    };

    const createGame = () => {
        if (!webSocketRef.current) {
            console.error('No WebSocket connection');
            return;
        }
        // hmm I guess this will be a http request not a websocket request
        webSocketRef.current.send(JSON.stringify({
            type: LOBBY_MOVE_TYPE.CREATE,
        }))
    }

    const joinGame = (gameId: string) => {
        if (!webSocketRef.current) {
            console.error('No WebSocket connection');
            return;
        }
        webSocketRef.current.send(JSON.stringify({
            type: LOBBY_MOVE_TYPE.JOIN,
            game_id: gameId
        }))
        setGameId(gameId);
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