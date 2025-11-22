import React, { useState, useEffect } from 'react';
import { useMode } from '../contexts/ModeContext';

export const NetworkStatus: React.FC<{ style?: React.CSSProperties }> = ({ style }) => {
    const { isOfflineMode, networkStatus } = useMode();
    const [cacheStatus, setCacheStatus] = useState<'unknown' | 'warming' | 'ready'>('unknown');

    useEffect(() => {
        // Check if service worker is ready and cache is warmed
        if ('serviceWorker' in navigator && navigator.serviceWorker.controller) {
            setCacheStatus('ready');
        } else if ('serviceWorker' in navigator) {
            setCacheStatus('warming');
            // Wait for service worker to be ready
            navigator.serviceWorker.ready.then(() => {
                setCacheStatus('ready');
            });
        }
    }, []);

    const warmCache = () => {
        if ('serviceWorker' in navigator && navigator.serviceWorker.controller) {
            setCacheStatus('warming');
            
            // Get current page resources
            const resourcesToCache: string[] = [];
            document.querySelectorAll('script[src]').forEach((element) => {
                const script = element as HTMLScriptElement;
                if (script.src.startsWith(window.location.origin)) {
                    resourcesToCache.push(script.src);
                }
            });
            document.querySelectorAll('link[href]').forEach((element) => {
                const link = element as HTMLLinkElement;
                if (link.href.startsWith(window.location.origin)) {
                    resourcesToCache.push(link.href);
                }
            });
            
            navigator.serviceWorker.controller.postMessage({
                type: 'WARM_CACHE',
                urls: resourcesToCache
            });
            
            setTimeout(() => setCacheStatus('ready'), 2000);
        }
    };

    if (networkStatus === 'checking') {
        return (
            <div style={{
                display: 'flex',
                alignItems: 'center',
                gap: '4px',
                fontSize: '12px',
                color: '#FFA500',
                ...style
            }}>
                <span>🔄</span>
                <span>Checking connection...</span>
            </div>
        );
    }

    return (
        <div style={{
            display: 'flex',
            alignItems: 'center',
            gap: '8px',
            fontSize: '12px',
            color: isOfflineMode ? '#FF6B6B' : '#4ECDC4',
            ...style
        }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '4px' }}>
                <span>{isOfflineMode ? '🔌' : '🌐'}</span>
                <span>{isOfflineMode ? 'Offline Mode' : 'Online'}</span>
            </div>
            
            {cacheStatus === 'warming' && (
                <span style={{ color: '#FFA500' }}>⚡ Caching...</span>
            )}
            
            {cacheStatus === 'ready' && !isOfflineMode && (
                <button
                    onClick={warmCache}
                    style={{
                        background: 'rgba(255,255,255,0.1)',
                        border: '1px solid rgba(255,255,255,0.2)',
                        color: 'white',
                        fontSize: '10px',
                        padding: '2px 6px',
                        borderRadius: '4px',
                        cursor: 'pointer'
                    }}
                    title="Pre-cache resources for offline use"
                >
                    📱 PWA Ready
                </button>
            )}
            
            {cacheStatus === 'ready' && isOfflineMode && (
                <span style={{ color: '#4ECDC4' }}>📱 Cached</span>
            )}
        </div>
    );
};