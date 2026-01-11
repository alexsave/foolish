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
import { Text } from './Text';

export const GameDisplay = () => {
    const game = useServer().game as PersonalGame;

    usePreventScroll();

    if (!game || !game.players || !game.players.length) {
        return <div><Text id="loading" /></div>;
    }

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            <KeyboardInputHandler />
            <BackButton />
            <CoverArrows />

            <div className="flex flex-1 flex-center">
                <p className="title--game-display">{game.name}</p>

                <DragShadow />
                <DeckAndFlipped />
                <DiscardPile />
                <ActionButtons />

                <div className="absolute flex flex-col items-center justify-center w-full" style={{ top: 0, bottom: 0 }}>
                    <DefenderShield />
                    <TableBattles />
                </div>

                <PlayerRing />
                <Chat />
            </div>

            <AnimationOverlay />
        </div>
    );
};