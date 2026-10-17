// Where the cards of one step fly from and to.
//
// Split out of AnimationOverlay, whose layout effect held all of this as
// closures. None of it is React: it reads the step the kernel's plan is on, the
// board and the row on screen, and the DOM, and answers with the flights to
// draw. The overlay is the shell that asks at the right moment (its layout
// effect) and renders the answer (FlightCard).
//
// NOT PURE IN THE STRICT SENSE, and the order matters: `planFlights` measures
// the page (some of it by forcing a reflow), stamps flight ids with the clock,
// and remembers slot geometry and table landings in the two maps below. It is
// called once per step, from a layout effect, and the measurements are taken in
// the order they are written.
import { covered, sameCard, seatKey, type TableView, type ViewBattle as Battle, type ViewCard as Card } from '../../state/view';
import type { ClientAnimationEvent } from '../../state/animationStep';
import type { AnimatedCard } from './FlightCard';
// The kernel's can_cover: web.wasm is loaded before any screen that reaches this
// renders - /, /[game_id], /dashboard, /history and /tutorial are each wrapped in
// KernelGate (src/components/KernelGate.tsx), the replay and the tutorial included.
import { canCoverPair } from '../../utils/gameValidation';
// The angle the battle grid lays a cover across its attack at: a flight has to
// land where the grid will DRAW the card, and the grid is where that is stated.
// `glideOffset` is the other half of that: while the row is still sliding to
// make room for the pile that just landed, a cell is not yet where it is going.
// And the box one battle takes in the row, which a measuring placeholder has
// to be the same size as.
import { BATTLE_CELL_STYLE, COVER_ROTATION_RAD, glideOffset } from './TableBattles';
import { handLandingCentre, laidOutOnScreen } from './handShape';

// Table-slot geometry cache (Stage 9). The on-table battle layout is a function of
// only (how many battle slots there are, the viewport size) - the 4th slot in a
// 4-slot table sits in the same place as the 4th slot in any other 4-slot table, so
// once measured we never need to re-measure that (count, index) at that viewport.
// This lets us skip the expensive measure path below (create placeholders → force a
// synchronous reflow → read rects → remove) on every attack after the first of each
// table size. The viewport is part of the KEY so a resize can't return stale
// coordinates; we also clear the whole cache on resize to bound its size and drop
// anything a layout/theme change may have shifted.
export const tableSlotPositionCache = new Map<string, { x: number; y: number }>();
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

// WHERE A TABLE ELEMENT WILL COME TO REST, which is not where it is drawn while
// the battle row is gliding (TableBattles' FLIP: the piles already down slide
// aside over the plan's own duration to make room for the one that just
// landed). A flight is measured at the START of its beat and lands at the end
// of it, by which time that slide is over - so a LANDING is aimed here, and a
// card LIFTING OFF keeps `centreOf`, because it leaves from where the eye has
// it. Zero for every element outside a gliding cell, which is all of them
// whenever nothing is moving.
const restingCentre = (element: HTMLElement): Spot => {
    const c = centreOf(element);
    const off = glideOffset(element);
    return { x: c.x - off.x, y: c.y - off.y };
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

/** Where a flight ends AND at what angle the place it ends at draws the card:
 *  a cover comes down laid across its attack, everything else lands flat. */
type Landing = { spot: Spot; angle: number };

/** A landing the place draws upright, which is every landing but a cover's. */
const flat = (spot: Spot): Landing => ({ spot, angle: 0 });

/** A card lying ON THE TABLE, as a selector: the grid's cell (TableBattles tags
 *  it data-location="table") and the card drawn inside it. The same card in a
 *  hand is not where a table flight starts or ends. */
const tableCardSelector = (suit: number, value: number): string =>
    `[data-location="table"] [data-card="${suit}-${value}"]`;

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
            const cardSelector = tableCardSelector(cardSuit, cardValue);
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

// Helper function to create invisible placeholders and measure their positions.
// `shownBattles` is the row the grid is drawing (the overlay's `shownRow`): the
// slots already taken are counted off it.
const measurePlaceholderPositions = (shownBattles: readonly Battle[], type: string, cards: readonly Card[], player_id?: string): Map<string, { x: number; y: number }> => {
    const positions = new Map<string, { x: number; y: number }>();

    if (type === 'attack_pass') {
        const currentBattleCount = shownBattles.length;
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
            // The grid's own cell box (TableBattles), so the slot measured is
            // the slot it will draw; unseen and untouchable on top of that.
            Object.assign(placeholder.style, BATTLE_CELL_STYLE, { visibility: 'hidden', pointerEvents: 'none' });
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

// Helper function to get fallback positions when elements aren't found.
//
// NOT FOR THE STOCK OR THE TRUMP'S SLOT. Those two had corners here
// ({100,120} and {100,180}), and the corner is what hid the bug that the
// stock's place vanished the instant its last cards left it: every such
// flight came out of the top-left of the window and nothing said why. Both
// places are now on screen for as long as a card can leave or reach them,
// on the kernel's word (ViewRules.show_deck_spot / show_flipped_slot), and
// are looked up with `stockPlace`, which has no floor at all.
const getFallbackPosition = (location: string, playerId?: string): { x: number; y: number } => {
    const centerX = window.innerWidth / 2;
    const centerY = window.innerHeight / 2;

    switch (location) {
        case 'hand':
            return { x: centerX, y: window.innerHeight - 100 };
        case 'table':
            return { x: centerX, y: centerY };
        case 'discard':
            return { x: window.innerWidth - 100, y: 120 }; // Top-right corner
        default:
            return { x: centerX, y: centerY };
    }
};

// THE STOCK'S PLACE AND THE TRUMP'S SLOT, where a card leaves or lands.
// DeckAndFlipped draws both for as long as a flight can use them, because
// the kernel says to (client_view_rules), so a miss here is a bug and is
// said out loud: the card gets no flight - it appears where it lands, which
// is what it would do with no animation at all - rather than one out of a
// made-up corner.
const stockPlace = (location: 'deck' | 'flipped'): Spot | null => {
    const el = findElementByLocation(location);
    if (el) return centreOf(el);
    console.error(`AnimationOverlay: no [data-location="${location}"] on screen for a card to fly from or to`);
    return null;
};

/** WHAT ONE STEP'S ENDPOINTS ARE WORKED OUT FROM. These were the variables the
 *  overlay's effect closed over; every function below that used to be one of
 *  its closures takes this and names the fields it reads, so what each one
 *  depends on is on its first line. Built once per step by `planFlights`. */
type Aim = {
    /** The step in flight (AnimationContext's currentAnimation). */
    step: ClientAnimationEvent;
    /** Its cards, which a step that flies anything has. */
    cards: readonly Card[];
    /** The acting seat's hand, as the page names it (data-player-id). */
    player_id: string | undefined;
    /** The board on screen. */
    game: TableView | null | undefined;
    /** The row the grid is actually drawing (the overlay's `shownRow`). */
    shownBattles: readonly Battle[];
    /** Every card of this step already has its cell in the grid. */
    allLaid: boolean;
    /** The placeholder measure, by card index; empty when `allLaid`. */
    measuredPositions: Map<string, { x: number; y: number }>;
    /** My hand's laid-out set while this flight is in the air (`handSlot`). */
    myLaidOut: readonly string[];
    /** For the guess of last resort (`coverTarget`): the attacks this batch claimed. */
    targetedAttackCards: Set<string>;
};

// WHERE THIS CARD'S CELL ALREADY IS, if the grid has one for it.
//
// The grid gives an incoming pile its cell when the step OPENS, not when
// it lands (src/state/animPlan.ts arrivingPiles), so by the time this
// effect runs the post-move row is laid out and this card's own slot is
// a thing on screen to measure. That IS the analytical answer iMessage
// aims a flight with: the browser has resolved the row the landing will
// confirm, and `restingCentre` takes the glide that is carrying the
// other cells there back off, so what comes out is where the cell COMES
// TO REST and not where it is drawn half a flight early.
const laidOnTable = (card: Card): HTMLElement | null =>
    document.querySelector(tableCardSelector(card.suit, card.value)) as HTMLElement | null;

// WHERE A CARD COMES TO REST IN MY HAND: the kernel's slot for the
// set the hand lays out while this flight is in the air - its own
// cards, then these at the end (handShape.tsx) - read off the hand's
// bottom edge, so a pickup that splits the hand into two rows lands
// every card on the slot it will rest in, while the cards already
// there are still sliding to theirs. iMessage's handLandingSlot is
// the same answer. Opponents' mini-hands have no plane: null.
const handSlot = ({ step: { to_location }, player_id, myLaidOut }: Aim, card: Card) => (to_location === 'hand' && player_id
    ? handLandingCentre(player_id, myLaidOut, card) : null);

// WHICH ATTACK THIS COVER CARD IS FOR - THE KERNEL'S ANSWER.
//
// Every cover the kernel pushes names it: evwire writes the covered
// attack and its battle index on the event (c/src/evwire.c's
// ENGINE_HOOK_COVER, one event per pair), and pushSequence.ts puts
// both on the step as `target_card` and `battle_index`. A multi-card
// cover of MY OWN is predicted before any push exists, and names its
// pairs in `target_cards` instead.
//
// iMessage answers the same question the same way: it finds the
// flying card in the board the kernel handed it and flies to THAT
// battle's cell (`openReplayFlights`,
// ios/FoolishKit/Boards/MessageTableView+OpenReplay.swift). It never
// re-runs the legality rule to pick a pile, and neither does this.
//
// The legality guess is the LAST resort, for an event carrying
// neither - and it is only ever a guess: it takes the FIRST uncovered
// battle this card could legally cover, which is the wrong pile
// whenever more than one is coverable (a trump over several attacks,
// equal ranks). `canCoverPair` is the kernel's can_cover, so even the
// guess asks the engine for the rule; what it cannot ask is which
// pile the player chose.
//
// STATEFUL, and deliberately so: two guessed cover cards in one
// flight must not both aim at the same attack, so a battle claimed
// here is struck off for the rest of the batch. That is why this is
// called once per card, in order, rather than mapped lazily.
const coverTarget = ({ step: { target_card, target_cards }, game, shownBattles, targetedAttackCards }: Aim, c: Card, i: number): Card | null => {
    if (target_cards && target_cards[i]) return target_cards[i];
    if (target_card) return target_card;
    if (!game) return null;

    const battle = shownBattles
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
//
// A card lying on the table is drawn at the grid's own tilt - the
// cover laid across at +COVER_ROTATION_RAD, the attack under it
// turned -COVER_ROTATION_RAD (TableBattles) - so a card leaving the
// table lifts off at that tilt and flattens as it flies, instead of
// snapping upright the instant the flight replaces it. iMessage
// carries the same value as its flight's `fromAngle`
// (ios/FoolishKit/Boards/MessageTableView+OpenReplay.swift).
const tableTilt = (shownBattles: readonly Battle[], card: Card): number => {
    const battle = shownBattles.find((b) => sameCard(b.attack, card)
        || (covered(b) && sameCard(b.defense, card)));
    if (!battle || !covered(battle)) return 0;
    return sameCard(battle.attack, card) ? -COVER_ROTATION_RAD : COVER_ROTATION_RAD;
};

const liftoff = ({ step: { from_location, trump_out, trump_from }, cards, player_id, shownBattles }: Aim, card: Card, index: number): { spot: Spot; fromLanding: boolean; angle: number } | null => {
    // The stock and the trump's slot have no floor (`stockPlace`).
    // The trump a draw deals out is its LAST card and leaves from
    // where the kernel says (`trump_from`), not from the stock.
    if (trump_out && trump_from && index === cards.length - 1) {
        const spot = stockPlace(trump_from);
        return spot && { spot, fromLanding: false, angle: 0 };
    }
    if (from_location === 'deck') {
        const spot = stockPlace('deck');
        return spot && { spot, fromLanding: false, angle: 0 };
    }

    let sourceElement: HTMLElement | null = null;
    let remembered: Spot | undefined;

    if (from_location === 'hand') {
        sourceElement = findElementByLocation('hand', player_id, card.suit, card.value);
    } else if (from_location === 'table') {
        sourceElement = document.querySelector(tableCardSelector(card.suit, card.value)) as HTMLElement | null;
        remembered = sourceElement ? undefined : tableLandings.get(`${card.suit}-${card.value}`);
        if (!sourceElement && !remembered) sourceElement = findElementByLocation('table', undefined, card.suit, card.value);
    }

    return {
        spot: spotFrom([() => sourceElement, () => remembered],
            getFallbackPosition(from_location || 'hand', player_id)),
        fromLanding: !sourceElement && !!remembered,
        angle: from_location === 'table' && sourceElement ? tableTilt(shownBattles, card) : 0,
    };
};

// WHERE A CARD LANDS - one function per destination, because a
// destination is where the knowledge about it belongs. Each is
// a chain plus a floor; none of them nests.

// The trump's slot is on screen while a card is on its way into
// it (ViewRules.show_flipped_slot), so it is measured, never guessed.
const landsFlipped = (): Spot | null => stockPlace('flipped');

// THE ATTACK THIS COVER LANDS ON, as an element to measure.
//
// The kernel's battle slot first - it names the slot outright and
// TableBattles tags the attack with it - then the target card's own
// rect, which survives a board whose slots have shifted under the
// flight. Neither query falls back to "some other battle": an attack
// this screen cannot show is the unmeasured case, and aiming at
// battle 0 because battle 3 has not rendered is the very mistake
// this path is here to stop. `battle_index` belongs to an event that
// carries ONE cover card (evwire emits one COVER event per pair), so
// a multi-card prediction uses its per-card targets instead.
const attackUnder = ({ step: { battle_index }, cards }: Aim, target: Card | null): HTMLElement | null =>
    (cards.length === 1 && battle_index !== undefined
        ? document.querySelector(`[data-location="table"] [data-battle-index="${battle_index}"]`) as HTMLElement | null
        : null)
    ?? (target
        ? document.querySelector(tableCardSelector(target.suit, target.value)) as HTMLElement | null
        : null);

// WHERE THE GRID WILL DRAW THE COVER, given that attack. A cover
// shares its attack's slot and its bottom edge and is laid across it
// at COVER_ROTATION_RAD about `center bottom` (TableBattles), so the
// cover's centre is the attack's centre turned through that angle
// about that point. Measured off the attack's own rect and the grid's
// own constant - this file keeps no offset of its own. Aiming at the
// attack's centre instead leaves the settle the flight is supposed to
// remove: measured in a browser, a cover drawn at (687, 425) over an
// attack whose uncovered centre is (680, 424).
//
// MEASURED AT REST AND UPRIGHT, and it has to be both. The rect is
// the axis-aligned bounding box, so it is the attack's own box only
// while the grid is drawing the card straight; the counter-tilt
// waits FLIGHT_ARM_MS before it starts (TableBattles), which is
// after this layout effect has run, so what is measured here is the
// untilted box. `glideOffset` takes off the other displacement - a
// row still sliding under the flight - because the cover lands when
// that slide has finished.
const laidAcross = (attack: HTMLElement): Spot => {
    const r = attack.getBoundingClientRect();
    const off = glideOffset(attack);
    return {
        x: r.left - off.x + r.width / 2 + Math.sin(COVER_ROTATION_RAD) * (r.height / 2),
        y: r.bottom - off.y - Math.cos(COVER_ROTATION_RAD) * (r.height / 2),
    };
};

// The table is the one destination that cares HOW the card got
// there: a cover aims at the attack it answers, an attack aims
// at the slot it will occupy, anything else aims at the table.
// A cover is also the one landing that is not flat - it comes down
// laid across, so the flight turns into that angle on the way in.
const landsOnTable = (aim: Aim, card: Card, index: number): Landing => {
    const { step: { type }, player_id, shownBattles, measuredPositions } = aim;
    if (type === 'cover') {
        const attack = attackUnder(aim, coverTarget(aim, card, index));
        // An attack we could measure is aimed at exactly; without one
        // we aim at the table and fan the cards 70px apart so
        // simultaneous covers do not stack on one point. The fan
        // belongs ONLY to the unmeasured case, and a card we could
        // not place is not laid across anything either.
        if (attack) return { spot: laidAcross(attack), angle: COVER_ROTATION_RAD };
        return {
            spot: spotFrom([() => {
                const table = findElementByLocation('table');
                return table ? shifted(restingCentre(table), index * 70, 0) : null;
            }], getFallbackPosition('table', player_id)),
            angle: 0,
        };
    }

    if (type === 'attack_pass') {
        const slot = shownBattles.length + index;
        return flat(spotFrom([
            // THE CELL THE GRID HAS ALREADY LAID OUT FOR THIS CARD,
            // which is now the ordinary case and not the rare one:
            // the row makes room for a pile when its step opens, so
            // the slot this card is flying into exists before the
            // flight does (`allLaid` in planFlights; iMessage says the same
            // of its own table flights, "the cell this card is
            // flying into does not exist until the step advances the
            // row - one paint before this builder first runs",
            // ios/FoolishKit/Boards/MessageTableView+OpenReplay.swift).
            // A board that got ahead of its own flight reaches the
            // same line by the other road and wants the same answer.
            () => {
                const laid = laidOnTable(card);
                return laid ? restingCentre(laid) : null;
            },
            () => measuredPositions.get(`${index}`),
            () => {
                const drop = findElementByLocation('table', undefined, undefined, undefined, slot);
                return drop ? restingCentre(drop) : null;
            },
            // The 60px fan is the FLOOR's alone: a drop zone we
            // actually found is already in the right place.
        ], shifted(getFallbackPosition('table', player_id), slot * 60, 0)));
    }

    return flat(spotFrom([() => {
        const table = findElementByLocation('table');
        return table ? restingCentre(table) : null;
    }], getFallbackPosition('table', player_id)));
};

// My hand's kernel slot first (`handSlot`) - a card the hand already
// holds (a refused card never left the board's hand) has one too, at
// its own index, and it is the settled answer where the card's own
// element may still be mid-slide. Then that element, for a hand with
// no plane; opponents' mini-hands fall through to their container.
const landsInHand = (aim: Aim, card: Card): Spot => {
    const { player_id } = aim;
    return spotFrom([
        () => handSlot(aim, card),
        () => (player_id
            ? document.querySelector(`[data-location="hand"][data-player-id="${player_id}"][data-card="${card.suit}-${card.value}"]`) as HTMLElement | null
            : null),
        () => findElementByLocation('hand', player_id),
    ], getFallbackPosition('hand', player_id));
};

const landsElsewhere = ({ step: { to_location }, player_id }: Aim): Spot => spotFrom(
    [() => (to_location === 'discard' ? findElementByLocation('discard') : null)],
    getFallbackPosition(to_location || 'table', player_id));

const landingFor = (aim: Aim, card: Card, index: number): Landing | null => {
    const { to_location } = aim.step;
    if (to_location === 'flipped') {
        const spot = landsFlipped();
        return spot && flat(spot);
    }
    return to_location === 'table' ? landsOnTable(aim, card, index)
        : to_location === 'hand' ? flat(landsInHand(aim, card))
            : flat(landsElsewhere(aim));
};

// ANOTHER SEAT'S DRAW: one stack of backs for the cards out of the
// stock, and - when the draw takes the flipped trump - the trump
// itself, face up, out of its own slot. It lay face up for
// everyone, so it flies face up for everyone; the kernel names it
// (`trump_out`, read off the boards, not off these backs) and says
// it is the last of the draw's cards.
const flightsOfBacks = (step: ClientAnimationEvent, cards: readonly Card[], player_id: string | undefined): AnimatedCard[] => {
    const { type, is_revert, trump_out, trump_from } = step;
    const endPos = spotFrom([() => findElementByLocation('hand', player_id)],
        getFallbackPosition('hand', player_id));
    const fromStock = trump_out ? cards.length - 1 : cards.length;
    const flights: AnimatedCard[] = [];
    const stockStart = fromStock > 0 ? stockPlace('deck') : null;
    if (stockStart) flights.push({
        id: `sanitized-refill-${player_id}-${Date.now()}`,
        card: { suit: -1, value: -1 }, // Keep original sanitized card
        startPosition: stockStart,
        endPosition: endPos,
        progress: 0,
        animationType: type,
        playerId: player_id,
        isSanitizedRefill: true,
        cardCount: fromStock,
        isRevert: is_revert,
        flight: step,
    });
    const trumpStart = trump_out && trump_from ? stockPlace(trump_from) : null;
    if (trump_out && trumpStart) flights.push({
        id: `${trump_out.suit}-${trump_out.value}-${player_id}-${Date.now()}-trump`,
        card: trump_out,
        startPosition: trumpStart,
        endPosition: { ...endPos },
        progress: 0,
        animationType: type,
        playerId: player_id,
        isRevert: is_revert,
        flight: step,
    });

    return flights;
};

// Render individual CardFaces for normal cards
const flightsOfCards = (aim: Aim): AnimatedCard[] => {
    const { step, cards, player_id, allLaid, measuredPositions } = aim;
    const { type, to_location, is_revert } = step;
    const newAnimatedCards: AnimatedCard[] = [];

    cards.forEach((card, index) => {
        // A place the kernel keeps on screen that is not there has
        // been said in the console; this card gets no flight.
        const lift = liftoff(aim, card, index);
        const landing = lift && landingFor(aim, card, index);
        if (!lift || !landing) return;
        const { spot: startPos, fromLanding, angle: fromAngle } = lift;
        const { spot: endPos, angle } = landing;

        // Small offset so simultaneous cards into the same UNMEASURED
        // area don't fully overlap; measured targets (table slots, hand
        // slots) are exact - offsetting them would re-introduce drift.
        const preciselyMeasured = (to_location === 'hand' && handSlot(aim, card) !== null) ||
            (type === 'attack_pass' && (allLaid || measuredPositions.get(`${index}`) !== undefined));
        if (!preciselyMeasured) {
            const stackOffset = index * 3;
            endPos.x += stackOffset;
            endPos.y += stackOffset;
        }

        if (to_location === 'table') tableLandings.set(`${card.suit}-${card.value}`, { ...endPos });

        newAnimatedCards.push({
            fromLanding,
            angle,
            fromAngle,
            id: `${card.suit}-${card.value}-${player_id}-${Date.now()}-${index}`,
            card,
            startPosition: startPos,
            endPosition: endPos,
            progress: 0,
            animationType: type,
            playerId: player_id,
            isRevert: is_revert,
            flight: step,
        });
    });

    return newAnimatedCards;
};

/**
 * THE FLIGHTS OF ONE STEP: every card it moves, with both ends of its journey
 * found. `step` is the step the run is on, `game` the board on screen and
 * `shownBattles` the row the grid is actually drawing (the overlay's
 * `shownRow`). Null for a step that flies nothing - a message, or one with no
 * cards - which the overlay answers by clearing what it shows; an empty list
 * is a different answer (cards, none of which could be placed).
 *
 * To be called from a layout effect, before the browser paints the commit that
 * started the flight, once per step: it measures the page as it stands.
 */
export const planFlights = (step: ClientAnimationEvent, game: TableView | null | undefined, shownBattles: readonly Battle[]): AnimatedCard[] | null => {
    // `target_card` and `battle_index` are the KERNEL's answer to "which
    // attack does this cover answer, and in which slot" - evwire carries both
    // on every COVER event and src/state/pushSequence.ts puts them on the
    // step. They are read below (`coverTarget`, `landsOnTable`); nothing in
    // this file decides a target any more except for an event that carries
    // none at all.
    const { type, cards, to_location, seat } = step;
    // The page names a seat's hand by its player id (data-player-id): the id the
    // event's own board gives its seat, or the board on screen's for a flight
    // that carries none (a move of mine, a revert).
    const eventBoard = step.game_state ?? game;
    const player_id = seat === undefined ? undefined : seatKey(eventBoard, seat);

    // Magic transitions are just messages, and every other type needs cards
    if (type === 'magic_transition' || !cards || cards.length === 0) {
        return null;
    }

    // Check if cards are sanitized (refill from other players)
    const isSanitized = cards.every(card => card.suit === -1 && card.value === -1);

    // Every card of this step already has one (`laidOnTable`) - so the
    // placeholder measure below has nothing left to answer, and inserting
    // placeholders into a row that already holds these cells would measure a
    // row one slot too long. It stays for the case that has no cell: a step
    // the grid is not laying a pile out for (a prediction with no board of its
    // own that the row cannot place, a fallback path).
    const allLaid = type === 'attack_pass' && to_location === 'table'
        && cards.every((c) => laidOnTable(c) !== null);

    // Measure placeholder positions for precise targeting
    const measuredPositions = allLaid
        ? new Map<string, { x: number; y: number }>()
        : measurePlaceholderPositions(shownBattles, type, cards, player_id);

    if (isSanitized) return flightsOfBacks(step, cards, player_id);

    return flightsOfCards({
        step, cards, player_id, game, shownBattles, allLaid, measuredPositions,
        // The set my hand lays out while this flight is in the air (`handSlot`).
        myLaidOut: to_location === 'hand' && player_id ? laidOutOnScreen(player_id, cards) : [],
        // For the guess of last resort (`coverTarget`): the attacks this batch claimed.
        targetedAttackCards: new Set<string>(),
    });
};
