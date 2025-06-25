
import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { LOBBY_MOVE_TYPE } from '../common/common';
//import supabase from '../db/supabaseClient';

const ServerContext = createContext<ServerContextType|null>(null);

interface ServerContextType {
    serverLogin: (name: string) => void;
}

// for now we'll just use a fake auth impl
export const ServerProvider = ({ children }: { children: React.ReactNode }) => {
    const [user, setUser] = useState(null);
    const [loading, setLoading] = useState(true);

    //useref websocket for sure
    const webSocketRef = useRef<WebSocket | null>(null);

    useEffect(() => {
        const ws = new WebSocket('ws://localhost:3001');
        ws.onmessage = (event) => {
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


    return (
        <ServerContext.Provider value={{
            serverLogin
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