import React, { useEffect, useState } from 'react';
import { useParams, useRouter } from 'next/navigation';
import { useServer } from '../contexts/ServerContext';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';
import { WinScreen } from './WinScreen';
import { GameLoadingPlaceholder } from './GameLoadingPlaceholder';
import { GAME_STATUS } from '@shared/types.ts';
import { initClientGuards, guardsReady } from '../wasm/clientGuards';

export const GameView = () => {
    const { game, gameLoadError } = useServer();
    const urlGameId = useParams<{ game_id: string }>().game_id?.toLowerCase() || null;
    const router = useRouter();

    // The interactive board (GameDisplay) gates buttons and predicts optimistic
    // state with the rules kernel (guards.wasm), which must be instantiated
    // before its first synchronous call. Kick it off on mount so the ~1ms
    // compile overlaps the game fetch; it's virtually always ready by the time
    // a PLAYING game arrives. Lobby / WinScreen don't touch the kernel.
    const [kernelReady, setKernelReady] = useState(guardsReady());
    useEffect(() => {
        if (kernelReady) return;
        let cancelled = false;
        initClientGuards()
            .then(() => { if (!cancelled) setKernelReady(true); })
            .catch((e) => console.error('guards.wasm failed to load:', e));
        return () => { cancelled = true; };
    }, [kernelReady]);

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

    // Game state not loaded yet (e.g. just navigated here from "create new game",
    // or a fresh page load): show a lobby-shaped placeholder instead of a blank
    // screen so it looks like the game is coming to life, not frozen.
    if (!game) {
        return <GameLoadingPlaceholder />;
    }

    // Conditionally render based on game status
    if (game.status === GAME_STATUS.WAITING) {
        return <Lobby />;
    } else if (game.status === GAME_STATUS.GAME_OVER) {
        return <WinScreen />;
    } else {
        // Hold the interactive board until the rules kernel is ready (normally
        // already true — it loads in ~1ms during the game fetch).
        if (!kernelReady) return <GameLoadingPlaceholder />;
        return <GameDisplay />;
    }
}; 