import { useEffect, useLayoutEffect, useRef } from "react";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { useTutorialHint } from "../../contexts/TutorialHintContext";
import { useAnimation } from "../../contexts/AnimationContext";
// The flight's own timing curve: the card coming down and the card turning out
// from under it are one movement, so they are written with one curve.
import { EASE, FLIGHT_ARM_MS } from "./FlightCard";
import { covered, sameCard, type ViewBattle, type ViewCard } from "../../state/view";
import { MOVE_ATTACK, MOVE_COVER, MOVE_PASS } from "@sdk/ts/gen/view_layout.bots.ts";
import type { ClientPlay } from "@sdk/ts/table/client_table.ts";

/** The angle a cover is laid across its attack at, in radians, about the card's
 *  `center bottom`. Exported because the flight that brings the cover in has to
 *  land where this grid will draw it: a cover shares its attack's slot and its
 *  bottom edge, so turning the attack's centre through this angle about that
 *  point IS the cover's centre (AnimationOverlay's `laidAcross`). The overlay
 *  reads the constant rather than keeping an offset of its own. */
export const COVER_ROTATION_RAD: number = Math.PI / 16;

const COVER_ROTATION: string = COVER_ROTATION_RAD + 'rad';

/** The name one pile keeps across a re-lay-out: its attack, which is what a
 *  battle IS until something covers it. Both the React key and the key the
 *  glide below remembers a cell's place by. */
const cellKey = (b: ViewBattle): string => b.attack.value + ' ' + b.attack.suit;

const STILL = { x: 0, y: 0 };

/** The translate a cell is drawn with AT THIS INSTANT. Read off the COMPUTED
 *  style, never the inline one: while a glide is running the inline transform
 *  is already the value it is heading for (nothing), and the interpolated
 *  value is the only one that says where the cell actually is. */
const liveTranslate = (el: HTMLElement): { x: number; y: number } => {
    const t = typeof getComputedStyle === 'function' ? getComputedStyle(el).transform : '';
    if (!t || t === 'none') return STILL;
    const n = t.slice(t.indexOf('(') + 1, t.lastIndexOf(')')).split(',').map(parseFloat);
    if (t.startsWith('matrix3d')) return { x: n[12] || 0, y: n[13] || 0 };
    if (t.startsWith('matrix')) return { x: n[4] || 0, y: n[5] || 0 };
    if (t.startsWith('translate')) return { x: n[0] || 0, y: n[1] || 0 };
    return STILL;
};

/**
 * HOW FAR A TABLE ELEMENT IS FROM WHERE IT WILL COME TO REST - zero unless the
 * grid is mid-glide (see the layout effect below).
 *
 * Exported for the overlay, which measures this grid to aim flights at it. A
 * flight is aimed at where its card will BE WHEN IT LANDS, and a pile that is
 * still sliding to make room is not there yet: measuring it raw would put a
 * cover down where its attack was half a flight ago. A card LIFTING OFF is the
 * other way round - it leaves from where it is drawn - so this is applied to
 * landings only, and AnimationOverlay says so at each of them.
 */
export const glideOffset = (el: HTMLElement): { x: number; y: number } => {
    const cell = typeof el.closest === 'function'
        ? el.closest('[data-location="table"]') as HTMLElement | null : null;
    return cell ? liveTranslate(cell) : STILL;
};

export const TableBattles = () => {
    const game = useServer().view;
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    const { user_id } = useAuth();
    const { coverMap, setCoverMap, isSelectingCover, selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();
    const hint = useTutorialHint();
    // The flight on screen, how long the kernel says it lasts (its plan's
    // AnimPlanStep.duration_ms), how long the ROW's own move lasts, and the row
    // itself while the kernel is holding it back. See `coveringNow` and the
    // glide below.
    const { currentAnimation, isAnimating, flightMs, rowMs, heldBattles } = useAnimation();

    // THE ROW THIS GRID DRAWS. The board's, except that a pile this run is
    // still carrying gets no cell until it lands - a move that ADDS a pile (a
    // pass, a throw-in) can reach a board that already holds it, and the grid
    // centres its cells, so every pile already down would sit half a slot plus
    // its gap to the side from the first painted frame, with a cold open having
    // no previous layout to move it back from
    // (src/state/animPlan.ts heldRow, c/src/anim_plan.h AnimCounts).
    const battles: readonly ViewBattle[] = heldBattles ?? game?.battles ?? [];

    // What a drop would do if it landed right now, and how many empty slots the
    // grid has to hold open for it - the cards the kernel's move actually lays.
    // Read before the glide below, which has to know about those slots: they
    // change the row's layout exactly as a landed pile does.
    let currentAction: ClientPlay | null = null;
    if (isDraggingForGameAction && draggedCard && currentCursorPos) {
        currentAction = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
    }
    const emptyDropZones = currentAction
        && (currentAction.moveType === MOVE_ATTACK || currentAction.moveType === MOVE_PASS)
        ? currentAction.cards.length : 0;

    // ---- THE ROW GLIDES, IT DOES NOT TELEPORT --------------------------------
    //
    // The cells are centred, so a pile landing beside them moves every one of
    // them - 40px at this card size - and CSS cannot transition a flex reflow:
    // the row simply appeared in its new arrangement on the frame the board
    // committed. iMessage never needed code for this (SwiftUI interpolates from
    // the layout already on screen); a browser does, and what it needs is a
    // FLIP - pin each cell where it is DRAWN, then let it travel to where the
    // new layout put it, over the plan's own duration for the landing that
    // moved it (`rowMs`). The interpolation and the screen coordinates are the
    // host's half of the boundary; the length of the move is the kernel's.
    const cellsRef = useRef(new Map<string, HTMLDivElement>());
    const restRef = useRef(new Map<string, { x: number; y: number }>());
    const rowMsRef = useRef(0);
    rowMsRef.current = rowMs;

    // A resize moves every cell without re-rendering anything, which would
    // leave the resting places below stale and glide the next landing from a
    // place no cell was ever at. Forgetting them means the next change simply
    // does not glide, which is the safe half of the trade.
    useEffect(() => {
        const cells = restRef.current;
        const onResize = () => cells.clear();
        window.addEventListener('resize', onResize);
        return () => window.removeEventListener('resize', onResize);
    }, []);

    // What can move a cell, as one string: the piles in order, and the drop
    // zones a drag is holding open beside them. The effect is inert on every
    // other render - a drag publishes a cursor position on every pointer move,
    // and a forced reflow per pointer move is not free.
    const layoutKey = battles.map(cellKey).join('|') + '#' + emptyDropZones;

    useLayoutEffect(() => {
        const cells = cellsRef.current;
        const was = restRef.current;

        // Where each cell is drawn RIGHT NOW: the resting place it was last
        // measured at, plus whatever is left of a glide still running. A glide
        // interrupted by the next landing carries on from where the eye has it,
        // never from where it was going.
        const drawn = new Map<string, { x: number; y: number }>();
        for (const [key, el] of cells) {
            const rest = was.get(key);
            if (!rest) continue;
            const off = liveTranslate(el);
            drawn.set(key, { x: rest.x + off.x, y: rest.y + off.y });
            if (off.x !== 0 || off.y !== 0) {
                el.style.transition = 'none';
                el.style.transform = '';
            }
        }

        // ...and where THIS layout puts it, with every glide taken back off.
        const rest = new Map<string, { x: number; y: number }>();
        for (const [key, el] of cells) {
            const r = el.getBoundingClientRect();
            rest.set(key, { x: r.left, y: r.top });
        }
        restRef.current = rest;

        const moved: HTMLDivElement[] = [];
        for (const [key, el] of cells) {
            const from = drawn.get(key);
            const to = rest.get(key);
            if (!from || !to) continue;   // a pile that was not down has nowhere to come from
            const dx = from.x - to.x, dy = from.y - to.y;
            if (Math.abs(dx) < 0.5 && Math.abs(dy) < 0.5) continue;
            el.style.transition = 'none';
            el.style.transform = `translate(${dx}px, ${dy}px)`;
            moved.push(el);
        }
        if (moved.length === 0) return;

        // One forced reflow, so the pinned place is the style the transition
        // starts FROM rather than a value the browser coalesces away
        // (intentional unused read).
        // eslint-disable-next-line @typescript-eslint/no-unused-expressions
        moved[0].offsetHeight;
        const ms = rowMsRef.current;
        for (const el of moved) {
            el.style.transition = `transform ${ms}ms ${EASE}`;
            el.style.transform = '';
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [layoutKey]);

    // Handle case where game is not loaded yet
    if (!game) {
        return <div></div>;
    }

    // Would the current drop cover this attack? The kernel's move names the
    // attacks it covers, so a single cover and a multi-cover are one case.
    const isCardBeingCovered = (attackCard: { suit: number; value: number }) =>
        currentAction?.moveType === MOVE_COVER
        && currentAction.attackCards.some((c) => sameCard(c, attackCard));

    // IS A COVER IN THE AIR FOR THIS BATTLE RIGHT NOW?
    //
    // The attack turns out of the way as the card covering it LEAVES THE HAND,
    // not once that card has landed: the two rotate together, over the one
    // flight, on the one curve. iMessage wrote this rule down after the same bug
    // - `coverTilted` is true while the cover is `flyingNow`, animated
    // `.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)`
    // (ios/FoolishKit/Boards/FBattleGrid.swift) - while the web had the attack
    // starting its turn only after the landing, riding CardFace's generic 0.2s
    // hover transition, which nobody chose for this.
    //
    // WHICH battle is the KERNEL's answer and not a guess: a pushed cover names
    // the attack it answers and its battle index (evwire -> pushSequence), and my
    // own predicted multi-cover names its pairs.
    const coveringNow = (attack: ViewCard, index: number): boolean => {
        const flight = currentAnimation;
        if (!isAnimating || !flight || flight.type !== 'cover') return false;
        if (flight.target_card) return sameCard(flight.target_card, attack);
        if (flight.target_cards) return flight.target_cards.some((c) => sameCard(c, attack));
        return flight.battle_index === index;
    };

    return <div data-table-container style={{
        display: 'flex',
        flexWrap: 'wrap',
        justifyContent: 'center',
        gap: '10px',
        width: '100%',
        maxWidth: '300px',
        margin: '0 auto'
    }}> {
        battles.map((battle, index) => {
            let containerStyle: React.CSSProperties = {
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                position: 'relative',
                width: '60px', // Slightly wider to accommodate rotation
                height: '80px', // Taller to accommodate stacked/rotated cards
                margin: '5px',
                justifyContent: 'center',
            };

            if (Array.from(coverMap.values()).some(c => c.value === battle.attack.value && c.suit === battle.attack.suit)) {
                containerStyle.border = '3px solid red';
            }

            // Highlight cards that would be covered
            if (isCardBeingCovered(battle.attack)) {
                containerStyle.border = '3px solid #d29002';
                containerStyle.backgroundColor = 'rgba(210, 144, 2, 0.1)';
            }

            // Tutorial: green-outline the attack the learner should cover.
            if (hint?.targetCard &&
                hint.targetCard.suit === battle.attack.suit &&
                hint.targetCard.value === battle.attack.value && !covered(battle)) {
                containerStyle.border = '3px solid #2fcf63';
                containerStyle.boxShadow = '0 0 14px 2px rgba(47,207,99,0.7)';
                containerStyle.borderRadius = '8px';
            }

            // Determine if this battle is covered
            const isCovered = covered(battle);
            // ...and whether it is turning out of the way of a cover that is
            // still in the air (see `coveringNow`): the tilt starts with the
            // flight, not with the landing.
            const tilted = isCovered || coveringNow(battle.attack, index);

            // Card styles with rotation around bottom center
            const attackCardStyle: React.CSSProperties = {
                position: 'absolute',
                bottom: '5px', // Position at bottom of container
                left: '50%', // Center horizontally
                transform: tilted
                    ? `translateX(-50%) rotate(-${COVER_ROTATION})`
                    : 'translateX(-50%)', // Just center if not covered
                transformOrigin: 'center bottom', // Rotate around bottom center of card
                // The cover's own flight, exactly: the kernel's duration for the
                // step on screen, the flight's own curve, and the same beat the
                // flight waits before it is armed (FLIGHT_ARM_MS - the overlay
                // paints the card where it stands for one frame first), so the
                // two cards turn at the same speed AND start together. 0ms when
                // nothing is flying, which is the only time this tilt changes
                // without a card causing it.
                //
                // Written out rather than left to CardFace's generic
                // `transform 0.2s ease-in-out, opacity 0.2s, box-shadow 0.2s`,
                // which is a hover/selection transition the tilt was only
                // riding. The selection's box-shadow keeps its own; opacity is
                // deliberately not here, because the veil must SNAP - a table
                // card fading out beside its own flying ghost reads as the card
                // dissolving (the same rule iMessage's FBattleGrid states).
                transition: `transform ${flightMs}ms ${EASE} ${flightMs > 0 ? FLIGHT_ARM_MS : 0}ms, box-shadow 0.2s ease-in-out`,
                zIndex: isCovered ? 1 : 2, // Attack goes behind when covered
            };

            const defenseCardStyle: React.CSSProperties = {
                position: 'absolute',
                bottom: '5px', // Position at bottom of container
                left: '50%', // Center horizontally  
                transform: `translateX(-50%) rotate(${COVER_ROTATION})`,
                transformOrigin: 'center bottom', // Rotate around bottom center of card
                zIndex: 2, // Defense always on top when present
            };

            // The cell is what GLIDES when the row re-lays out, so the layout
            // effect above needs it by the same name React keys it by.
            const key = cellKey(battle);
            return <div
                key={key}
                ref={(el) => {
                    if (el) cellsRef.current.set(key, el);
                    else cellsRef.current.delete(key);
                }}
                style={containerStyle}
                data-location="table"
            >
                <CardFace
                    data-battle-index={index}
                    data-card={`${battle.attack.suit}-${battle.attack.value}`}
                    card={battle.attack}
                    owner="table"
                    style={attackCardStyle}
                    onClick={() => isSelectingCover && setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack))) }
                />
                {isCovered && (
                    <CardFace 
                        card={battle.defense} 
                        owner="table" 
                        style={defenseCardStyle}
                        data-card={`${battle.defense.suit}-${battle.defense.value}`} 
                    />
                )}
            </div>
        })
    }
    {/* Render empty drop zones for attack/pass actions */}
    {Array.from({ length: emptyDropZones }, (_, index) => (
        <div 
            key={`empty-${index}`} 
            data-location="table"
            data-battle-index={battles.length + index}
            style={{
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                position: 'relative',
                width: '60px',
                height: '80px',
                margin: '5px',
                justifyContent: 'center',
                border: '3px dashed #d29002',
                backgroundColor: 'rgba(210, 144, 2, 0.1)',
                borderRadius: '8px'
            }}
        >
            {/* Empty placeholder for new card */}
            <div style={{
                width: '40px',
                height: '60px',
                border: '2px dashed #d29002',
                borderRadius: '4px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                fontSize: '24px',
                color: '#d29002',
                opacity: 0.7
            }}>
                +
            </div>
        </div>
    ))}
    </div>
};