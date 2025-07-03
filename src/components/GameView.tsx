import React, { useEffect } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useServer } from '../contexts/ServerContext';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';

export const GameView = () => {
    const { game, gameLoadError } = useServer();
    const urlGameId = useParams().game_id?.toLowerCase() || null;
    const navigate = useNavigate();

    // Handle game load errors - redirect to dashboard
    useEffect(() => {
        if (gameLoadError && gameLoadError === urlGameId) {
            console.log('Game not found, redirecting to dashboard');
            navigate('/dashboard');
        }
    }, [gameLoadError, urlGameId, navigate]);

    // Handle error state - game doesn't exist
    if (gameLoadError === urlGameId) {
        return <div>Game not found. Redirecting to dashboard...</div>;
    }

    // Handle missing game data
    if (!game) {
        return <div>Loading...</div>;
    }
    
    // Conditionally render based on game status
    if (game.status === 'waiting') {
        return <Lobby />;
    } else {
        return <GameDisplay />;
    }
}; 