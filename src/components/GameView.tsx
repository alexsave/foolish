import React, { useEffect, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useServer } from '../contexts/ServerContext';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';
import { WinScreen } from './WinScreen';
import { GAME_STATUS } from '../common/types';
import { generateFernPattern } from '../utils/fernFractal';

export const GameView = () => {
    const { game, gameLoadError } = useServer();
    const urlGameId = useParams().game_id?.toLowerCase() || null;
    const navigate = useNavigate();
    const [fernPattern, setFernPattern] = useState<string>('');

    // Debug: Generate fern pattern
    useEffect(() => {
        console.log('Generating fern pattern for debug...');
        generateFernPattern().then((dataUrl: string) => {
            console.log('Fern pattern generated:', dataUrl.substring(0, 50) + '...');
            setFernPattern(dataUrl);
        }).catch((error) => {
            console.error('Error generating fern pattern:', error);
        });
    }, []);

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
    
    // Debug fern pattern display
    const debugFernDiv = (
        <div style={{
            position: 'fixed',
            top: '50%',
            left: '50%',
            transform: 'translate(-50%, -50%)',
            width: '200px',
            height: '280px',
            backgroundImage: fernPattern ? `url(${fernPattern})` : undefined,
            backgroundSize: '100% 100%',
            backgroundRepeat: 'no-repeat',
            border: '2px solid #ff0000',
            backgroundColor: '#000',
            zIndex: 9999
        }}>
            {!fernPattern && <div style={{ color: 'white', padding: '10px' }}>Generating fern...</div>}
        </div>
    );

    // Conditionally render based on game status
    if (game.status === GAME_STATUS.WAITING) {
        return <>{debugFernDiv}<Lobby /></>;
    } else if (game.status === GAME_STATUS.GAME_OVER) {
        return <>{debugFernDiv}<WinScreen /></>;
    } else {
        return <>{debugFernDiv}<GameDisplay /></>;
    }
}; 