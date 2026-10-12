import { useEffect, useLayoutEffect, useState, useRef } from 'react';
import { useAnimation } from '../../contexts/AnimationContext';
import type { ViewBattle as Battle } from '../../state/view';
import { FlightCard, FLIGHT_ARM_MS, type AnimatedCard } from './FlightCard';
import { useServer } from '../../contexts/ServerContext';
import { shownRow } from '../../state/animPlan';
// WHERE the cards of a step fly from and to is worked out there, off the DOM
// and the kernel's plan; this component is the shell that asks at the right
// moment (the layout effect below) and draws the answer.
import { planFlights, tableSlotPositionCache } from './flightGeometry';

/** A board that is not there yet has no row. */
const EMPTY_ROW: readonly Battle[] = [];

export const AnimationOverlay = () => {
    const [animatedCards, setAnimatedCards] = useState<AnimatedCard[]>([]);
    // `flightMs` is the kernel's duration for the step on screen (its plan's
    // AnimPlanStep.duration_ms), not a constant this file keeps: the curve and
    // the interpolation are rendering, the length of the flight is not.
    const { currentAnimation, isAnimating, flightMs, heldPiles, arrivingPiles } = useAnimation();
    const { view: game } = useServer();
    // THE ROW THE GRID IS ACTUALLY DRAWING. The board's, minus the piles of the
    // steps this run has not started and plus the piles of the step it is flying
    // now (TableBattles, and src/state/animPlan.ts heldPiles / arrivingPiles).
    // Everything in this file that counts slots, picks a pile or asks how a card
    // is lying has to ask the row on SCREEN - aiming at a cell the grid is not
    // drawing is aiming at nothing.
    const shownBattles = shownRow(game?.battles ?? EMPTY_ROW, heldPiles, arrivingPiles);
    const overlayRef = useRef<HTMLDivElement>(null);

    // Invalidate the table-slot geometry cache on resize (Stage 9). The cache key
    // already includes the viewport, so a resized window can't read stale
    // coordinates; clearing on top of that bounds the map's size and drops entries a
    // reflow/theme shift may have moved.
    useEffect(() => {
        const onResize = () => tableSlotPositionCache.clear();
        window.addEventListener('resize', onResize);
        return () => window.removeEventListener('resize', onResize);
    }, []);

    // Built in a layout effect, before the browser paints the commit that started
    // the flight: that commit hides the card where it stands (CardFace), and the
    // flight must show it in the same frame. The flight lives exactly as long as its
    // event: the commit that lands it (AnimationContext) takes it off the overlay in
    // the frame the board shows the card where it landed.
    useLayoutEffect(() => {
        if (!currentAnimation || !isAnimating) {
            setAnimatedCards((prev) => (prev.length === 0 ? prev : []));
            return;
        }

        // Every endpoint is measured here, in this effect, in the order
        // flightGeometry.ts takes them. Null is a step that flies nothing (a
        // message, or one with no cards).
        const flights = planFlights(currentAnimation, game, shownBattles);
        if (!flights) {
            setAnimatedCards((prev) => (prev.length === 0 ? prev : []));
            return;
        }
        setAnimatedCards(flights);


        // Use CSS transitions - much smoother than manual animation. Progress goes to 1
        // after a short delay that crosses a browser paint, so the start frame renders
        // before the transition begins; only this flight's cards move.
        const flight = currentAnimation;
        const begin = setTimeout(() => {
            setAnimatedCards(prev => prev.map(animatedCard => (animatedCard.flight === flight
                ? { ...animatedCard, progress: 1 } // This triggers the CSS transition
                : animatedCard)));
        }, FLIGHT_ARM_MS);
        return () => clearTimeout(begin);

        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [currentAnimation, isAnimating]);

    if (animatedCards.length === 0) {
        return null;
    }

    return (
        <div 
            ref={overlayRef}
            style={{
                position: 'fixed',
                top: 0,
                left: 0,
                width: '100%',
                height: '100%',
                pointerEvents: 'none',
                zIndex: 10000,
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
            } as React.CSSProperties}
        >
            {animatedCards.map((flight) => (
                <FlightCard key={flight.id} flight={flight} flightMs={flightMs} />
            ))}
        </div>
    );
}; 