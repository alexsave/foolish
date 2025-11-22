// Context for automatic network detection and offline/online mode management
import React, { createContext, useContext, useState, useEffect } from 'react';

interface ModeContextType {
    isOfflineMode: boolean;
    isOnlineMode: boolean;
    networkStatus: 'online' | 'offline' | 'checking';
}

const ModeContext = createContext<ModeContextType | null>(null);

export const ModeProvider = ({ children }: { children: React.ReactNode }) => {
    const [networkStatus, setNetworkStatus] = useState<'online' | 'offline' | 'checking'>('checking');

    useEffect(() => {
        // Initial check
        const checkNetworkStatus = async () => {
            try {
                // Try to fetch a small resource to test connectivity
                // We'll try to fetch the manifest.json which should be cached by service worker
                const response = await fetch('/manifest.json', { 
                    cache: 'no-cache',
                    signal: AbortSignal.timeout(5000) // 5 second timeout
                });
                setNetworkStatus(response.ok ? 'online' : 'offline');
            } catch (error) {
                setNetworkStatus('offline');
            }
        };

        checkNetworkStatus();

        // Listen for online/offline events
        const handleOnline = () => {
            setNetworkStatus('online');
        };

        const handleOffline = () => {
            setNetworkStatus('offline');
        };

        window.addEventListener('online', handleOnline);
        window.addEventListener('offline', handleOffline);

        // Periodic connectivity check (every 30 seconds when online)
        const intervalId = setInterval(() => {
            if (networkStatus === 'online') {
                checkNetworkStatus();
            }
        }, 30000);

        return () => {
            window.removeEventListener('online', handleOnline);
            window.removeEventListener('offline', handleOffline);
            clearInterval(intervalId);
        };
    }, [networkStatus]);

    const contextValue: ModeContextType = {
        isOfflineMode: networkStatus === 'offline',
        isOnlineMode: networkStatus === 'online',
        networkStatus
    };

    return (
        <ModeContext.Provider value={contextValue}>
            {children}
        </ModeContext.Provider>
    );
};

export const useMode = () => {
    const context = useContext(ModeContext);
    if (!context) {
        throw new Error('useMode must be used within a ModeProvider');
    }
    return context;
};