import { PersonalGame } from '../common/types';
import { useServer } from '../contexts/ServerContext';
import { TableBattles } from './GameDisplay/TableBattles';
import { PlayerRing } from './GameDisplay/PlayerRing';
import { DefenderShield } from './GameDisplay/DefenderShield';
import { ActionButtons } from './GameDisplay/ActionButtons';
import { DeckAndFlipped } from './GameDisplay/DeckAndFlipped';
import { DiscardPile } from './GameDisplay/DiscardPile';
import { DragShadow } from './GameDisplay/DragShadow';
import { CoverArrows } from './GameDisplay/CoverArrows';
import { Chat } from './GameDisplay/Chat';
import { AnimationOverlay } from './GameDisplay/AnimationOverlay';
import { KeyboardInputHandler } from './KeyboardInputHandler';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { BackButton } from './BackButton';

export const GameDisplay = () => {
    const game = useServer().game as PersonalGame;

    usePreventScroll();

    // Handle missing game data (GameView handles loading and error states)
    if (!game || !game.players || !game.players.length) {
        return <div>Loading GameDisplay...</div>;
    }

    return <div
        data-game-container
        style={{
            //backgroundColor: 'transparent',
            width: '100%',
            height: '100vh',
            touchAction: 'none', 
        }}
    >
        <WoolBackgroundLayer />
        <KeyboardInputHandler />
        
        <BackButton />

        <CoverArrows />

        <div style={{ display: 'flex', flex: 1, justifyContent: 'center', alignItems: 'center' }}>
            <p style={{ color: 'white', fontSize: '24px', fontWeight: 'bold' }}>{game.name}</p>

            <DragShadow />

            <DeckAndFlipped />

            <DiscardPile />

            <ActionButtons />

            <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
                <DefenderShield />

                <TableBattles />
            </div>

            <PlayerRing />

            <Chat />

        </div>

        <AnimationOverlay />

    </div>
};