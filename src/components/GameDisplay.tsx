import { PersonalGame } from '../common/types';
import { useServer } from '../contexts/ServerContext';
import { TableBattles } from './GameDisplay/TableBattles';
import { PlayerRing } from './GameDisplay/PlayerRing';
import { DefenderShield } from './GameDisplay/DefenderShield';
import { ActionButtons } from './GameDisplay/ActionButtons';
import { DeckAndFlipped } from './GameDisplay/DeckAndFlipped';
import { DragShadow } from './GameDisplay/DragShadow';
import { CoverArrows } from './GameDisplay/CoverArrows';
import { Chat } from './GameDisplay/Chat';
import { usePreventScroll } from '../hooks/usePreventScroll';

export const GameDisplay = () => {
    const game = useServer().game as PersonalGame;

    usePreventScroll();

    // Handle missing game data (GameView handles loading and error states)
    if (!game || !game.players || !game.players.length) {
        return <div>Loading...</div>;
    }

    return <div
        data-game-container
        style={{
            backgroundColor: '#982621',
            width: '100%',
            height: '100vh',
            touchAction: 'manipulation', // Allow taps and pinch-zoom but prevent double-tap zoom and panning
        }}
    >
        <CoverArrows />

        <div style={{ display: 'flex', flex: 1, justifyContent: 'center', alignItems: 'center' }}>
            <p style={{ color: 'white', fontSize: '24px', fontWeight: 'bold' }}>{game.name}</p>

            <DragShadow />

            <DeckAndFlipped />

            <ActionButtons />

            <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
                <DefenderShield />

                <TableBattles />
            </div>

            <PlayerRing />

            <Chat />

        </div>

    </div>
};