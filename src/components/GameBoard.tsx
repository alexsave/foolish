import React from 'react';
import { PersonalGame } from '@shared/core/types.ts';
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
import { KeyboardPlayMode } from './GameDisplay/KeyboardPlayMode';
import { Text } from './Text';

/**
 * The one parameterized board behind every game-state source.
 *
 * Live `GameDisplay`, the replay `ReplayStage`, and the `TutorialBoard` are all
 * the same composition of the shared GameDisplay/* render pieces — they only
 * differ in WHICH pieces appear, how far the play area is inset (to clear each
 * screen's chrome), and the chrome itself. The render pieces read the game from
 * `useServer().game` (+ animation/auth/game/drag/hint contexts), so this layout
 * is source-agnostic: the live ServerProvider, the replay provider, and the
 * tutorial provider each supply a different implementation of that same read
 * surface, and capability flags below pick what to show.
 *
 * Differences are driven by:
 *  - `interactive`  — show the learner/live hand + wooden action buttons + drag
 *                     shadow + cover arrows (live, tutorial). Off for the replay
 *                     spectator, who only watches.
 *  - `showChat`     — the live in-game chat overlay (live only).
 *  - `showKeyboard` — keyboard play shortcuts (live only).
 *  - `title`        — centred game-name caption (live only).
 *  - `boardInset`   — CSS insets for the play-area wrapper so PlayerRing's
 *                     percentage seats clear each screen's bars/controls.
 * Slots inject each screen's bespoke chrome (replay transport + reveal overlay,
 * tutorial narration + hint + intro/end cards) on top of the shared board.
 */
export interface GameBoardProps {
    /** Render the self hand, wooden action buttons, drag shadow + cover arrows. */
    interactive?: boolean;
    /** Live in-game chat overlay + bubbles. */
    showChat?: boolean;
    /** Keyboard play shortcuts. */
    showKeyboard?: boolean;
    /** Centred game-name caption (live game). */
    title?: string;
    /** Insets for the play-area wrapper, so seats clear the screen's chrome. */
    boardInset?: React.CSSProperties;
    /** Extra chrome rendered above the board: transport, narration, overlays, etc. */
    chrome?: React.ReactNode;
    /** Extra content rendered inside the play-area wrapper (e.g. reveal-hands). */
    overlay?: React.ReactNode;
}

export const GameBoard = ({
    interactive = false,
    showChat = false,
    showKeyboard = false,
    title,
    boardInset,
    chrome,
    overlay,
}: GameBoardProps) => {
    const game = useServer().game as PersonalGame;

    if (!game || !game.players || !game.players.length) {
        return <div><Text id="loading" /></div>;
    }

    return (
        <>
            {showKeyboard && <KeyboardInputHandler />}
            {/* arrow-key play: navigate the hand + cover/pass targeting with a
                red cursor. Interactive screens only (the replay uses arrows for
                its own transport). */}
            {interactive && <KeyboardPlayMode />}
            {interactive && <CoverArrows />}

            {/* The play area. Default (live) fills its parent; replay/tutorial
                pass insets so PlayerRing's percentage seats clear their bars. */}
            <div className="flex flex-1 flex-center" style={boardInset}>
                {title && <p className="title--game-display">{title}</p>}

                {interactive && <DragShadow />}
                <DeckAndFlipped />
                <DiscardPile />
                {interactive && <ActionButtons />}

                <div
                    className="absolute flex flex-col items-center justify-center w-full"
                    style={{ top: 0, bottom: 0 }}
                >
                    <DefenderShield />
                    <TableBattles />
                </div>

                <PlayerRing />
                {showChat && <Chat />}
                {overlay}
            </div>

            <AnimationOverlay />
            {chrome}
        </>
    );
};
