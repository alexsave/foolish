import { useEffect } from 'react';
import { useRouter } from 'next/navigation';
import { useUrlGameId } from '../utils/useUrlGameId';
import { useServer } from '../contexts/ServerContext';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';
import { WinScreen } from './WinScreen';
import { GameLoading } from './GameLoading';
import { GAME_STATUS } from '../state/view';

export const GameView = () => {
    const { view: game, gameLoadError } = useServer();
    const urlGameId = useUrlGameId()?.toLowerCase() || null;
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

    // Game state not loaded yet (e.g. just navigated here from "create new game",
    // or a fresh page load): the lobby's name if this tab already knows it, and
    // the background alone if it does not.
    if (!game) {
        return <GameLoading gameId={urlGameId} />;
    }

    // Conditionally render based on game status
    if (game.status === GAME_STATUS.WAITING) {
        return <Lobby />;
    } else if (game.status === GAME_STATUS.GAME_OVER) {
        return <WinScreen />;
    } else {
        // The board's gates and optimistic moves are the kernel's, which is loaded
        // before this screen renders: /[game_id] is wrapped in KernelGate
        // (src/components/KernelGate.tsx).
        return <GameDisplay />;
    }
}; 