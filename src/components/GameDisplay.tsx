import { PersonalGame } from '../common/types';
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
    const game = useServer().game as PersonalGame;

    usePreventScroll();

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            <GameBoard
                interactive
                showChat
                showKeyboard
                title={game?.name}
                chrome={<BackButton />}
            />
        </div>
    );
};
