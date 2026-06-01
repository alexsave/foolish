import React, { useEffect } from 'react';
import { useParams, useRouter } from 'next/navigation';
import { useServer } from '../contexts/ServerContext';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';
import { WinScreen } from './WinScreen';
import { GAME_STATUS } from '../common/types';

export const GameView = () => {
    const { game, gameLoadError } = useServer();
    const urlGameId = useParams<{ game_id: string }>().game_id?.toLowerCase() || null;
    const router = useRouter();

    // Handle game load errors - redirect to dashboard
    useEffect(() => {
        if (gameLoadError && gameLoadError === urlGameId) {
            console.log('Game not found, redirecting to dashboard');
            router.push('/dashboard');
        }
    }, [gameLoadError, urlGameId, router]);

    // Game doesn't exist (redirecting) or not loaded yet: render nothing and
    // let the app-wide background show through.
    if (gameLoadError === urlGameId) {
        return null;
    }

    if (!game) {
        return null;
    }
    
    // Conditionally render based on game status
    if (game.status === GAME_STATUS.WAITING) {
        return <Lobby />;
    } else if (game.status === GAME_STATUS.GAME_OVER) {
        return <WinScreen />;
    } else {
        return <GameDisplay />;
    }
}; 