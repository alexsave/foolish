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
    /** The angle, in radians, the card LANDS at: the battle grid's cover tilt for
     *  a cover, 0 for everything that lands flat. */
    angle?: number;
    /** The angle it TAKES OFF at - the tilt the place it left was drawing it at. */
    fromAngle?: number;
}

// The card's own box. CardFace draws 50x70 border-box (its defaultStyle), and
// the flight is hung by its CENTRE: both endpoints are element centres
// (AnimationOverlay's centreOf), so a card hung by its corner would land half a
// card short of the slot it was measured into. `translate(-50%, -50%)` does the
// hanging and needs no width; the height is named because the TILT pivots about
// the card's bottom edge and the pivot has to be paid for (see `pivot` below).
const CARD_H = 70;

/** The flight's timing curve - and the one the battle grid tilts a covered
 *  attack on, so the cover and the card under it turn at the same speed
 *  (TableBattles). iMessage states the same four numbers, in
 *  `ios/FoolishKit/Boards/FBattleGrid.swift`'s `.timingCurve(0.25, 0.46, 0.45, 0.94)`. */
export const EASE = 'cubic-bezier(0.25, 0.46, 0.45, 0.94)';

/** How long a flight stands still before its transition is armed: one crossed
 *  paint, so the start frame renders where the card actually is (see `progress`
 *  below - AnimationOverlay arms it on this timer). Exported because anything
 *  that has to MOVE WITH a flight has to wait the same beat, or it leads the
 *  card it is supposed to be moving with: the battle grid's counter-tilt does
 *  (TableBattles). It is a paint, not choreography - the kernel owns the
 *  duration, and this is the browser's own latency before it begins. */
export const FLIGHT_ARM_MS = 25;

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
    // Scale up during the flight; a card taking off from where an earlier flight
    // left it starts at the size that one landed at.
    const scale = progress === 0 && fromLanding ? 1.8 : 1.5 + progress * 0.3;
    // THE CARD TURNS AS IT FLIES, from the tilt the place it left was drawing it
    // at into the tilt the place it lands draws it at - so a cover arrives
    // already laid across its attack instead of snapping flat-to-tilted on the
    // landing frame, and a cover picked up off the table lifts off still tilted
    // and flattens on the way. iMessage's ghost does exactly this, over the same
    // flight and about the same pivot
    // (ios/FoolishKit/Boards/BoardFlight.swift: `fromAngle + (angle - fromAngle) * p`,
    // `anchor: .bottom`); here the browser interpolates it.
    const angle = progress === 0 ? (flight.fromAngle ?? 0) : (flight.angle ?? 0);
    // AND IT PIVOTS ABOUT THE CARD'S BOTTOM EDGE, which is the pivot the battle
    // grid lays a cover about (`transformOrigin: 'center bottom'`, TableBattles):
    // turning about the centre instead would hand over to a card that had turned
    // about something else, and the two would not line up. The price is that the
    // scale and the tilt now both work off that corner rather than the middle, so
    // this translate puts the card's CENTRE back on the point the overlay
    // measured - at any angle, at any scale. It is the same two lines of
    // trigonometry AnimationOverlay's `laidAcross` aims with.
    const pivot = `translate(${-scale * (CARD_H / 2) * Math.sin(angle)}px, ${(scale * Math.cos(angle) - 1) * (CARD_H / 2)}px)`;

    return (
        <div
            style={{
                position: 'absolute',
                left: at.x,
                top: at.y,
                transformOrigin: 'center bottom',
                transform: `translate(-50%, -50%) ${pivot} scale(${scale}) rotate(${angle}rad)`,
                opacity: 1,
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
                // One curve for the whole flight, the grid's own: the card's
                // travel, its growth and its turn are one movement, and the
                // covered attack turning under it (TableBattles) rides the same
                // one. The transform used to ease-out on its own while the
                // position eased on this curve.
                transition: progress === 0
                    ? 'none' // the start frame must not animate into itself
                    : `left ${flightMs}ms ${EASE}, top ${flightMs}ms ${EASE}, transform ${flightMs}ms ${EASE}`,
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
