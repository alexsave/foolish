import { useEffect, useLayoutEffect, useState, useRef } from 'react';
import { useAnimation } from '../../contexts/AnimationContext';
import { covered, seatKey, type ViewCard as Card } from '../../state/view';
import { FlightCard, type AnimatedCard } from './FlightCard';
import { useServer } from '../../contexts/ServerContext';
// The kernel's can_cover: bots.wasm is loaded before any screen that reaches this
// renders - /, /[game_id], /dashboard, /history and /tutorial are each wrapped in
// KernelGate (src/components/KernelGate.tsx), the replay and the tutorial included.
import { canCoverPair } from '../../utils/gameValidation';

// Table-slot geometry cache (Stage 9). The on-table battle layout is a function of
// only (how many battle slots there are, the viewport size) - the 4th slot in a
// 4-slot table sits in the same place as the 4th slot in any other 4-slot table, so
// once measured we never need to re-measure that (count, index) at that viewport.
// This lets us skip the expensive measure path below (create placeholders → force a
// synchronous reflow → read rects → remove) on every attack after the first of each
// table size. The viewport is part of the KEY so a resize can't return stale
// coordinates; we also clear the whole cache on resize to bound its size and drop
// anything a layout/theme change may have shifted.
const tableSlotPositionCache = new Map<string, { x: number; y: number }>();
// A slot is identified by the table's total slot count + the slot's absolute index,
// at a given viewport.
const tableSlotKey = (totalSlots: number, slotIndex: number): string =>
    `${window.innerWidth}x${window.innerHeight}|${totalSlots}|${slotIndex}`;

// Where each card's last flight to the table landed, by card. A card whose move the
// server refused lands on the table in flight only - no board ever lays it - so its
// return flight starts from the spot the outbound flight left it at.
const tableLandings = new Map<string, { x: number; y: number }>();

/** A point on screen, in viewport coordinates. */
type Spot = { x: number; y: number };

/** Something that might tell us where a card goes: an element to measure, a
 *  position already known, or nothing. */
type Candidate = HTMLElement | Spot | null | undefined;

const centreOf = (element: HTMLElement): Spot => {
    const rect = element.getBoundingClientRect();
    return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
};

const isElement = (c: Candidate): c is HTMLElement =>
    !!c && typeof (c as HTMLElement).getBoundingClientRect === 'function';

// EVERY endpoint in this file is the same question asked of a different list:
// "where is this card going - and if the thing I would measure is not on
// screen, what is the next best answer?" That used to be written out longhand
// at each of the eight endpoints, as a ladder of if/else on whether each
// querySelector came back, which is why this file nested nine deep for logic
// that is three lines wide.
//
// The chain is LAZY on purpose. Several candidates are DOM queries or forced
// reflows (measurePlaceholderPositions), and the nearer candidate usually wins,
// so a strict list would pay for measurements it then throws away - on every
// card of every flight.
//
// `floor` is separate from the chain and is not optional: a flight with no
// endpoint is a card that flies to the top-left corner, so every caller has to
// say what happens when the screen cannot answer.
const spotFrom = (chain: Array<() => Candidate>, floor: Spot): Spot => {
    for (const next of chain) {
        const c = next();
        if (!c) continue;
        return isElement(c) ? centreOf(c) : { ...c };
    }
    return floor;
};

/** `spot` moved by (dx, dy). Returns a new object: callers mutate their result. */
const shifted = (spot: Spot, dx: number, dy: number): Spot => ({ x: spot.x + dx, y: spot.y + dy });


export const AnimationOverlay = () => {
    const [animatedCards, setAnimatedCards] = useState<AnimatedCard[]>([]);
    // `flightMs` is the kernel's duration for the step on screen (its plan's
    // AnimPlanStep.duration_ms), not a constant this file keeps: the curve and
    // the interpolation are rendering, the length of the flight is not.
    const { currentAnimation, isAnimating, flightMs } = useAnimation();
    const { view: game } = useServer();
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

    // Helper function to find element by data attributes with retry logic
    const findElementByLocation = (location: string, playerId?: string, cardSuit?: number, cardValue?: number, battleIndex?: number): HTMLElement | null => {
        // For table destinations, try specific targeting first
        if (location === 'table') {
            // If we have a battle index, try to find the specific battle container
            if (battleIndex !== undefined) {
                const battleSelector = `[data-battle-index="${battleIndex}"]`;
                const battleElement = document.querySelector(battleSelector) as HTMLElement | null;
                if (battleElement) {
                    return battleElement;
                }
            }
            
            // If we have specific card coordinates, try to find that card ON THE TABLE
            // (the same card in a hand is not where a table flight starts or ends)
            if (cardSuit !== undefined && cardValue !== undefined) {
                const cardSelector = `[data-location="table"] [data-card="${cardSuit}-${cardValue}"]`;
                const cardElement = document.querySelector(cardSelector) as HTMLElement | null;
                if (cardElement) {
                    return cardElement;
                }
            }
        }
        
        // First try to find the specific card (for current user's hand)
        if (playerId && cardSuit !== undefined && cardValue !== undefined) {
            const specificCardSelector = `[data-location="${location}"][data-player-id="${playerId}"][data-card="${cardSuit}-${cardValue}"]`;
            const specificElement = document.querySelector(specificCardSelector) as HTMLElement | null;
            if (specificElement) {
                return specificElement;
            }
        }
        
        // Fall back to general location area
        let generalSelector = `[data-location="${location}"]`;
        if (playerId) {
            generalSelector += `[data-player-id="${playerId}"]`;
        }
        
        return document.querySelector(generalSelector) as HTMLElement | null;
    };

    // Helper function to create invisible placeholders and measure their positions
    const measurePlaceholderPositions = (type: string, cards: readonly Card[], player_id?: string): Map<string, { x: number; y: number }> => {
        const positions = new Map<string, { x: number; y: number }>();

        if (type === 'attack_pass') {
            const currentBattleCount = game?.battles.length || 0;
            // After this attack lands the table has currentBattleCount + cards.length
            // slots; each new card targets the slot at (currentBattleCount + index).
            const totalSlots = currentBattleCount + cards.length;

            // Fast path: if every target slot's geometry is already cached for this
            // table size + viewport, return it and skip the placeholder/reflow work.
            const cached = new Map<string, { x: number; y: number }>();
            let allCached = true;
            for (let index = 0; index < cards.length; index++) {
                const hit = tableSlotPositionCache.get(tableSlotKey(totalSlots, currentBattleCount + index));
                if (!hit) { allCached = false; break; }
                cached.set(`${index}`, hit);
            }
            if (allCached) {
                return cached;
            }

            // Find the table battles container. The container itself is tagged
            // (data-table-container) so this also works when the table is EMPTY -
            // an opponent's first attack of a bout used to find no
            // [data-location="table"] child at all and fall back to a generic
            // center position instead of the real first slot.
            const tableBattlesElement = (document.querySelector('[data-table-container]')
                ?? document.querySelector('[data-location="table"]')?.parentElement) as HTMLElement | null;
            if (!tableBattlesElement) return positions;

            // Create invisible placeholder battle containers
            const placeholders: HTMLElement[] = [];
            cards.forEach((card, index) => {
                const placeholder = document.createElement('div');
                placeholder.style.cssText = `
                    display: flex;
                    flex-direction: column;
                    align-items: center;
                    position: relative;
                    width: 60px;
                    height: 80px;
                    margin: 5px;
                    justify-content: center;
                    visibility: hidden;
                    pointer-events: none;
                `;
                placeholder.setAttribute('data-placeholder', 'true');
                
                // Insert at the correct position (after existing battles)
                const existingBattles = tableBattlesElement.children;
                if (existingBattles.length > currentBattleCount + index) {
                    tableBattlesElement.insertBefore(placeholder, existingBattles[currentBattleCount + index]);
                } else {
                    tableBattlesElement.appendChild(placeholder);
                }
                
                placeholders.push(placeholder);
            });
            
            // Force layout and measure positions (intentional unused read).
            // eslint-disable-next-line @typescript-eslint/no-unused-expressions
            tableBattlesElement.offsetHeight;
            
            placeholders.forEach((placeholder, index) => {
                const rect = placeholder.getBoundingClientRect();
                const position = {
                    x: rect.left + rect.width / 2,
                    y: rect.top + rect.height / 2
                };
                positions.set(`${index}`, position);
                // Cache this slot's geometry keyed by (table size, absolute slot
                // index, viewport) so future attacks at the same size skip the reflow.
                tableSlotPositionCache.set(tableSlotKey(totalSlots, currentBattleCount + index), position);
            });

            // Clean up placeholders
            placeholders.forEach(placeholder => placeholder.remove());
        }
        
        return positions;
    };

    // Measure where cards ENTERING the local player's hand will actually land.
    // The rendered hand appends new cards at the END (displayedHand), but the
    // old targeting picked querySelector's FIRST hand-card match - so drawn
    // cards flew toward the leftmost card instead of their landing slot. Same
    // placeholder trick as the table slots: append invisible flex items with a
    // real card's flex geometry, reflow, measure, remove - the measured spots
    // include the squeeze the incoming cards cause. Returns [] for players
    // without a per-card hand in the DOM (opponents' mini-hands).
    const measureHandSlotPositions = (count: number, playerId?: string): { x: number; y: number }[] => {
        if (!playerId) return [];
        const container = document.querySelector(`[data-hand-container][data-player-id="${playerId}"]`) as HTMLElement | null;
        if (!container) return [];

        const placeholders: HTMLElement[] = [];
        for (let i = 0; i < count; i++) {
            const ph = document.createElement('div');
            // Same flex-item geometry as a hand card (ActionButtons): the
            // measured layout matches the hand once the drawn cards commit.
            ph.style.cssText = 'flex:1 1 0;min-width:20px;max-width:50px;height:70px;visibility:hidden;pointer-events:none;';
            ph.setAttribute('data-placeholder', 'true');
            container.appendChild(ph);
            placeholders.push(ph);
        }
        // Force layout and measure (intentional unused read).
        // eslint-disable-next-line @typescript-eslint/no-unused-expressions
        container.offsetHeight;
        const out = placeholders.map(ph => {
            const rect = ph.getBoundingClientRect();
            return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
        });
        placeholders.forEach(ph => ph.remove());
        return out;
    };

    // Helper function to get fallback positions when elements aren't found
    const getFallbackPosition = (location: string, playerId?: string): { x: number; y: number } => {
        const centerX = window.innerWidth / 2;
        const centerY = window.innerHeight / 2;
        
        switch (location) {
            case 'hand':
                return { x: centerX, y: window.innerHeight - 100 };
            case 'table':
                return { x: centerX, y: centerY };
            case 'deck':
                return { x: 100, y: 120 };
            case 'flipped':
                return { x: 100, y: 180 }; // Slightly below the deck
            case 'discard':
                return { x: window.innerWidth - 100, y: 120 }; // Top-right corner
            default:
                return { x: centerX, y: centerY };
        }
    };

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

        // target_card and battle_index are kept in the destructure for future
        // multi-card cover handling; currently unused.
        // eslint-disable-next-line @typescript-eslint/no-unused-vars
        const { type, cards, from_location, to_location, seat, target_card, target_cards, battle_index, is_revert } = currentAnimation;
        // The page names a seat's hand by its player id (data-player-id): the id the
        // event's own board gives its seat, or the board on screen's for a flight
        // that carries none (a move of mine, a revert).
        const eventBoard = currentAnimation.game_state ?? game;
        const player_id = seat === undefined ? undefined : seatKey(eventBoard, seat);

        // Magic transitions are just messages, and every other type needs cards
        if (type === 'magic_transition' || !cards || cards.length === 0) {
            setAnimatedCards((prev) => (prev.length === 0 ? prev : []));
            return;
        }

        // Check if cards are sanitized (refill from other players)
        const isSanitized = cards.every(card => card.suit === -1 && card.value === -1);

        // Measure placeholder positions for precise targeting
        const measuredPositions = measurePlaceholderPositions(type, cards, player_id);
        
        if (isSanitized) {
            // Render single CardBack for sanitized refill
            const startPos = spotFrom([() => findElementByLocation('deck')],
                getFallbackPosition('deck'));
            const endPos = spotFrom([() => findElementByLocation('hand', player_id)],
                getFallbackPosition('hand', player_id));

            const newAnimatedCard: AnimatedCard = {
                id: `sanitized-refill-${player_id}-${Date.now()}`,
                card: { suit: -1, value: -1 }, // Keep original sanitized card
                startPosition: startPos,
                endPosition: endPos,
                progress: 0,
                animationType: type,
                playerId: player_id,
                isSanitizedRefill: true,
                cardCount: cards.length,
                isRevert: is_revert,
                flight: currentAnimation,
            };

            setAnimatedCards([newAnimatedCard]);
        } else {
            // Render individual CardFaces for normal cards
            const newAnimatedCards: AnimatedCard[] = [];

            // Cards entering the local hand land at its END - measure those
            // slots once for the whole batch (deal/refill/pickup).
            const handSlots = to_location === 'hand'
                ? measureHandSlotPositions(cards.length, player_id)
                : [];

            // For cover animations, keep track of which attack cards have been targeted
            const targetedAttackCards = new Set<string>();

            // WHICH ATTACK THIS COVER CARD IS FOR.
            //
            // The event says so outright when it carries target_cards (a
            // multi-card cover names its pairs). When it does not, the board
            // has to work it out the way the kernel did: the first uncovered
            // battle this card can legally cover. `canCoverPair` is the
            // kernel's can_cover, so this asks the engine rather than
            // re-deciding the rule.
            //
            // STATEFUL, and deliberately so: two cover cards in one flight
            // must not both aim at the same attack, so a battle claimed here
            // is struck off for the rest of the batch. That is why this is
            // called once per card, in order, rather than mapped lazily.
            const coverTarget = (c: Card, i: number): Card | null => {
                if (target_cards && target_cards[i]) return target_cards[i];
                if (!game?.battles) return null;

                const battle = game.battles
                    .filter((b) => !covered(b))
                    .find((b) => canCoverPair(b.attack, c, game.powerSuit)
                        && !targetedAttackCards.has(`${b.attack.suit}-${b.attack.value}`));
                if (!battle) return null;

                targetedAttackCards.add(`${battle.attack.suit}-${battle.attack.value}`);
                return battle.attack;
            };

            // WHERE A CARD STARTS. Three origins, then the shared
            // resolution. `fromLanding` rides along because a card whose
            // move the server refused was never laid on the board - only a
            // flight put it there - so its return starts at the spot, and
            // the scale, that flight left it at.
            const liftoff = (card: Card): { spot: Spot; fromLanding: boolean } => {
                let sourceElement: HTMLElement | null = null;
                let remembered: Spot | undefined;

                if (from_location === 'hand') {
                    sourceElement = findElementByLocation('hand', player_id, card.suit, card.value);
                } else if (from_location === 'deck') {
                    sourceElement = findElementByLocation('deck');
                } else if (from_location === 'table') {
                    sourceElement = document.querySelector(`[data-location="table"] [data-card="${card.suit}-${card.value}"]`) as HTMLElement | null;
                    remembered = sourceElement ? undefined : tableLandings.get(`${card.suit}-${card.value}`);
                    if (!sourceElement && !remembered) sourceElement = findElementByLocation('table', undefined, card.suit, card.value);
                }

                return {
                    spot: spotFrom([() => sourceElement, () => remembered],
                        getFallbackPosition(from_location || 'hand', player_id)),
                    fromLanding: !sourceElement && !!remembered,
                };
            };

            // WHERE A CARD LANDS - one function per destination, because a
            // destination is where the knowledge about it belongs. Each is
            // a chain plus a floor; none of them nests.

            const landsFlipped = (): Spot => spotFrom([
                () => findElementByLocation('flipped'),
                // The trump sits 60px under the deck when its own element
                // has not rendered yet.
                () => {
                    const deck = findElementByLocation('deck');
                    return deck ? shifted(centreOf(deck), 0, 60) : null;
                },
            ], getFallbackPosition('flipped', player_id));

            // The table is the one destination that cares HOW the card got
            // there: a cover aims at the attack it answers, an attack aims
            // at the slot it will occupy, anything else aims at the table.
            const landsOnTable = (card: Card, index: number): Spot => {
                if (type === 'cover') {
                    const target = coverTarget(card, index);
                    // A known target is aimed at exactly; without one we aim
                    // at the table and fan the cards 70px apart so
                    // simultaneous covers do not stack on one point. The fan
                    // belongs ONLY to the untargeted case.
                    return target
                        ? spotFrom(
                            [() => findElementByLocation('table', undefined, target.suit, target.value)],
                            getFallbackPosition('table', player_id))
                        : spotFrom([() => {
                            const table = findElementByLocation('table');
                            return table ? shifted(centreOf(table), index * 70, 0) : null;
                        }], getFallbackPosition('table', player_id));
                }

                if (type === 'attack_pass') {
                    const slot = (game?.battles.length || 0) + index;
                    return spotFrom([
                        // The board already shows the card - a confirmation
                        // that beat its own flight - so that IS the landing.
                        () => document.querySelector(`[data-location="table"] [data-card="${card.suit}-${card.value}"]`) as HTMLElement | null,
                        () => measuredPositions.get(`${index}`),
                        () => findElementByLocation('table', undefined, undefined, undefined, slot),
                        // The 60px fan is the FLOOR's alone: a drop zone we
                        // actually found is already in the right place.
                    ], shifted(getFallbackPosition('table', player_id), slot * 60, 0));
                }

                return spotFrom([() => findElementByLocation('table')],
                    getFallbackPosition('table', player_id));
            };

            // The card's own place when the hand already holds it (a refused
            // card never left the board's hand; a board committed before the
            // flight shows it there), else the measured landing slot for the
            // local hand; opponents' mini-hands fall through to their
            // container.
            const landsInHand = (card: Card, index: number): Spot => spotFrom([
                () => (player_id
                    ? document.querySelector(`[data-location="hand"][data-player-id="${player_id}"][data-card="${card.suit}-${card.value}"]`) as HTMLElement | null
                    : null),
                () => handSlots[index],
                () => findElementByLocation('hand', player_id),
            ], getFallbackPosition('hand', player_id));

            const landsElsewhere = (): Spot => spotFrom(
                [() => (to_location === 'discard' ? findElementByLocation('discard') : null)],
                getFallbackPosition(to_location || 'table', player_id));

            const landingFor = (card: Card, index: number): Spot =>
                to_location === 'flipped' ? landsFlipped()
                    : to_location === 'table' ? landsOnTable(card, index)
                        : to_location === 'hand' ? landsInHand(card, index)
                            : landsElsewhere();

            cards.forEach((card, index) => {
                const { spot: startPos, fromLanding } = liftoff(card);
                const endPos = landingFor(card, index);

                // Small offset so simultaneous cards into the same UNMEASURED
                // area don't fully overlap; measured targets (table slots, hand
                // slots) are exact - offsetting them would re-introduce drift.
                const preciselyMeasured = (to_location === 'hand' && handSlots[index] !== undefined) ||
                    (type === 'attack_pass' && measuredPositions.get(`${index}`) !== undefined);
                if (!preciselyMeasured) {
                    const stackOffset = index * 3;
                    endPos.x += stackOffset;
                    endPos.y += stackOffset;
                }

                if (to_location === 'table') tableLandings.set(`${card.suit}-${card.value}`, { ...endPos });

                newAnimatedCards.push({
                    fromLanding,
                    id: `${card.suit}-${card.value}-${player_id}-${Date.now()}-${index}`,
                    card,
                    startPosition: startPos,
                    endPosition: endPos,
                    progress: 0,
                    animationType: type,
                    playerId: player_id,
                    isRevert: is_revert,
                    flight: currentAnimation,
                });
            });

            setAnimatedCards(newAnimatedCards);
        }


        // Use CSS transitions - much smoother than manual animation. Progress goes to 1
        // after a short delay that crosses a browser paint, so the start frame renders
        // before the transition begins; only this flight's cards move.
        const flight = currentAnimation;
        const begin = setTimeout(() => {
            setAnimatedCards(prev => prev.map(animatedCard => (animatedCard.flight === flight
                ? { ...animatedCard, progress: 1 } // This triggers the CSS transition
                : animatedCard)));
        }, 25);
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