import { PersonalGame } from '@api/core/types.ts';
import { useServer } from '../contexts/ServerContext';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { BackButton } from './BackButton';
import { GameBoard } from './GameBoard';

// Live game board: a thin wrapper that selects the live ServerProvider's
// capabilities (interactive hand + buttons, chat, keyboard, game-name title)
// and renders the shared GameBoard. The provider tree (ServerProvider,
// AnimationProvider + RealtimeAnimationFeed, GameProvider, DragProvider) is
// supplied higher up by ProtectedRoute.
export const GameDisplay = () => {
    const { game, staleRoundNotice } = useServer();
    const g = game as PersonalGame;

    usePreventScroll();

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            {staleRoundNotice && (
                // A small, transient, self-clearing toast: the move was refused
                // because a round closed before it landed (REJECT_STALE_ROUND).
                <div data-stale-round-notice role="status" style={{
                    position: 'fixed', top: 12, left: '50%', transform: 'translateX(-50%)',
                    zIndex: 1000, maxWidth: '90vw', padding: '8px 14px', borderRadius: 8,
                    background: 'rgba(20,20,20,0.92)', color: '#fff', fontSize: 14,
                    boxShadow: '0 2px 10px rgba(0,0,0,0.35)', pointerEvents: 'none', textAlign: 'center',
                }}>
                    {staleRoundNotice}
                </div>
            )}
            <GameBoard
                interactive
                showChat
                showKeyboard
                title={g?.name}
                chrome={<BackButton />}
            />
        </div>
    );
};
