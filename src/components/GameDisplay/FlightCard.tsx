// One card in flight: where it sits, how big it is, and how it is lit.
//
// Split out of AnimationOverlay, which was deciding WHERE a card goes and
// drawing it in the same component. Those are different jobs with different
// inputs: the overlay reads the DOM and the kernel's plan to work out two
// points, and this reads nothing at all - every value it paints comes off the
// AnimatedCard it is handed. That makes it the only part of the flight path
// that can be looked at, and changed, without thinking about measurement.
//
// NOTHING HERE DECIDES ANYTHING ABOUT THE GAME. The card's identity, its
// endpoints, whether it was a revert, and how long the flight lasts are all
// settled before this renders. `flightMs` in particular is the kernel's
// duration for the step (AnimPlanStep.duration_ms, via useAnimation), not a
// constant this file keeps - the curve and the interpolation are rendering,
// the length of the flight is not.
import type React from 'react';
import { CardFace } from './CardFace';
import { CardBack } from './CardBack';
import type { ViewCard as Card } from '../../state/view';

/** A card the overlay is flying, with both ends of its journey already found. */
export interface AnimatedCard {
    id: string;
    card: Card;
    startPosition: { x: number; y: number };
    endPosition: { x: number; y: number };
    progress: number;
    animationType: string;
    playerId?: string;
    isSanitizedRefill?: boolean;
    cardCount?: number;
    isRevert?: boolean; // Flag for reverted optimistic animations
    flight: object; // the event this card flies for
    fromLanding?: boolean; // starts where an earlier flight landed, at that flight's landing scale
}

// Half a card, so the flight is positioned by its CENTRE: both endpoints are
// element centres (AnimationOverlay's centreOf), and a card hung by its corner
// would land half a card short of the slot it was measured into.
const HALF_W = 35;
const HALF_H = 45;

const EASE = 'cubic-bezier(0.25, 0.46, 0.45, 0.94)';

/** The red treatment for a move the server refused. */
const REVERT = {
    border: '2px solid rgb(220, 38, 38)',
    filter: 'brightness(1.3) contrast(1.2) sepia(0.3) saturate(1.8) hue-rotate(-10deg)',
    backgroundColor: 'rgb(255, 150, 150)',
    shadow: 'rgba(255,0,0,0.6)',
} as const;

const NORMAL = {
    border: '2px solid black',
    filter: 'none',
    backgroundColor: 'var(--color-card-face)',
    shadow: 'rgba(0,0,0,0.4)',
} as const;

export const FlightCard = ({ flight, flightMs }: { flight: AnimatedCard; flightMs: number }) => {
    const { startPosition, endPosition, progress, card, isSanitizedRefill, cardCount, isRevert, fromLanding } = flight;

    // progress is 0 for exactly one frame - the card rendered where it stands,
    // before the transition is armed - then 1. The browser animates between
    // them; this never interpolates.
    const at = progress === 0 ? startPosition : endPosition;
    const look = isRevert ? REVERT : NORMAL;

    return (
        <div
            style={{
                position: 'absolute',
                left: at.x - HALF_W,
                top: at.y - HALF_H,
                // Scale up during the flight; a card taking off from where an
                // earlier flight left it starts at the size that one landed at.
                transform: `scale(${progress === 0 && fromLanding ? 1.8 : 1.5 + progress * 0.3})`,
                opacity: 1,
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
                transition: progress === 0
                    ? 'none' // the start frame must not animate into itself
                    : `left ${flightMs}ms ${EASE}, top ${flightMs}ms ${EASE}, transform ${flightMs}ms ease-out`,
            } as React.CSSProperties}
        >
            {isSanitizedRefill ? (
                // A refill from another player's deck: the cards are masked, so
                // one back stands for the whole draw.
                <CardBack deckSize={cardCount || 1} enableRandomRotation={false} />
            ) : (
                <CardFace
                    card={card}
                    isAnimationOverlay={true}
                    style={{
                        border: look.border,
                        boxShadow: `0 ${progress * 10}px ${progress * 20}px ${look.shadow}`,
                        filter: look.filter,
                        backgroundColor: look.backgroundColor,
                    }}
                />
            )}
        </div>
    );
};
