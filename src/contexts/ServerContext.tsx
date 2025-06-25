
import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { LOBBY_MOVE_TYPE } from '../common/common';
//import supabase from '../db/supabaseClient';

const ServerContext = createContext<ServerContextType|null>(null);

interface ServerContextType {
    serverLogin: (name: string) => void;
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

    //useref websocket for sure
    const webSocketRef = useRef<WebSocket | null>(null);

    useEffect(() => {
        const ws = new WebSocket('ws://localhost:3001');
        ws.onmessage = (event) => {
            const data = JSON.parse(event.data);
            if (data.type === 'game_created') {
                setGameId(data.game_id);
            }
            console.log(event.data);
        };
        ws.onopen = () => {
            console.log('Connected to server');
        };
        ws.onclose = () => {
            console.log('Disconnected from server');
        };
        ws.onerror = (event) => {
            console.log('Error', event);
        };
        webSocketRef.current = ws;
        return () => {
            ws.close();
        };
    }, []);

    const serverLogin = (name: string) => {
        if (!webSocketRef.current) {
            console.error('No WebSocket connection');
            return;
        }
        webSocketRef.current.send(JSON.stringify({
            type: LOBBY_MOVE_TYPE.LOGIN,
            player_name: name
        }))
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