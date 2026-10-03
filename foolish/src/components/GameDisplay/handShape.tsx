// handShape - where my hand's cards sit, which is the KERNEL's answer.
//
// The hand used to be a flex row that kept squeezing its cards (down to 20px,
// then off both screen edges). It now splits into two rows exactly the way the
// iMessage fan does, because both ask the same C rule: c/src/hand_layout.h
// hand_layout, reached through ClientTable.handLayout. This file holds the
// web's units for it, the one width rule, the set of cards the hand lays out,
// and the context that hands the answer to everything that has to move with it
// (the hand itself, the pills column, my seat badge).
//
// BOTTOM-ANCHORED, like iMessage's handLandingSlot. The hand stands on the
// bottom edge of its box and grows UPWARD when it splits, so a card is placed
// by its distance from that bottom edge (`slotBottom`): a split never moves the
// bottom row, and the top row slides up out of it instead of every card jumping
// a row's height in the frame the box grows.
import React, { createContext, useCallback, useContext, useLayoutEffect, useMemo, useState } from 'react';
import { clientTable, type HandLayout, type HandMetrics } from '@sdk/ts/table/client_table.ts';
import { useServer } from '../../contexts/ServerContext';
import { useAnimation } from '../../contexts/AnimationContext';
import type { ViewCard as Card } from '../../state/view';

/** The web hand's units, CSS px. One row stands card_h tall (row_pad 0), the
 *  height the flex row had; a card is never wider than 50 or narrower than 20,
 *  as before; 2px between cards, as the old 1px margins made; and the hand
 *  splits below iMessage's 34 (34 CSS px reads as 34pt), which at 390 wide is
 *  from 11 cards on. */
export const WEB_HAND: HandMetrics = {
    cardWMax: 50, cardWMin: 20, cardH: 70, gap: 2, rowGap: 6, rowPad: 0, splitBelow: 34,
};

/** The hand's gutter on each side before anything is measured
 *  (cards.css [data-hand-container], the safe area aside). */
const GUTTER = 10;

/** THE ONE WIDTH RULE. The hand lays out in its plane's width; before the plane
 *  has a width (the first render, and jsdom, where layout is all zeros) it is
 *  the window less the two gutters - the width the plane will have on every
 *  screen without a side safe area, so nothing balloons or jumps for a frame. */
export const handWidth = (plane: Element | null): number => {
    const w = plane ? plane.getBoundingClientRect().width : 0;
    return w > 0 ? w : Math.max(0, (typeof window === 'undefined' ? 0 : window.innerWidth) - 2 * GUTTER);
};

/** How `count` cards sit in `width` CSS px. */
export const webHandLayout = (count: number, width: number): HandLayout =>
    clientTable().handLayout(WEB_HAND, count, width);

/** A slot's distance from the hand's bottom edge (see the top of this file). */
export const slotBottom = (layout: HandLayout, i: number): number =>
    layout.rows.height - layout.slot[i].y - layout.slot[i].h;

const keyOf = (c: Card) => `${c.suit}-${c.value}`;

/** WHAT THE HAND LAYS OUT: the cards it holds, then the cards a flight is
 *  carrying into it right now, at the end, where the board that lands them puts
 *  them (displayedHand appends). iMessage lays out the same set (FHandFan,
 *  "present cards + whatever this step just opened"), so the hand makes room -
 *  and splits, if the arrivals take it over the line - while the cards are in
 *  the air, and each lands on the slot it will rest in. A card the hand already
 *  holds (a refused move coming home) is not counted twice. */
export const handLaidOut = (hand: readonly Card[], arriving: readonly Card[]): string[] =>
    withArrivals(hand.map(keyOf), arriving);

const withArrivals = (held: string[], arriving: readonly Card[]): string[] => {
    const out = [...held];
    const seen = new Set(out);
    for (const c of arriving) {
        const k = keyOf(c);
        if (c.suit < 0 || seen.has(k)) continue;
        seen.add(k);
        out.push(k);
    }
    return out;
};

/** Where `card` lands in MY hand, in client px (its centre), from the kernel's
 *  slot for the laid-out set - or null when that hand is not on screen. Read
 *  off the plane's BOTTOM edge, which a split cannot move, so the answer is
 *  the settled one even while the plane is still growing. */
export const handLandingCentre = (playerId: string, laidOut: readonly string[], card: Card): { x: number; y: number } | null => {
    const plane = document.querySelector(`[data-hand-container][data-player-id="${playerId}"] [data-hand-plane]`);
    const i = laidOut.indexOf(keyOf(card));
    if (!plane || i < 0) return null;
    const layout = webHandLayout(laidOut.length, handWidth(plane));
    const s = layout.slot[i];
    if (!s) return null;
    const r = plane.getBoundingClientRect();
    return { x: r.left + s.x + s.w / 2, y: r.bottom - slotBottom(layout, i) - s.h / 2 };
};

/** The hand's laid-out set as the page shows it right now: the hand's own
 *  cards in their on-screen order, then the cards in flight to it. */
export const laidOutOnScreen = (playerId: string, arriving: readonly Card[]): string[] =>
    withArrivals([...document.querySelectorAll(`[data-hand-container][data-player-id="${playerId}"] [data-location="hand"][data-card]`)]
        .sort((a, b) => Number(a.getAttribute('data-card-index')) - Number(b.getAttribute('data-card-index')))
        .map((e) => e.getAttribute('data-card')!), arriving);

export interface HandShape {
    /** The kernel's layout of the laid-out set. */
    layout: HandLayout;
    /** How much taller than one row the hand stands: what sits above it moves up by this. */
    lift: number;
    /** Attach to the hand's plane, the box the slots are measured in. */
    planeRef: (el: HTMLDivElement | null) => void;
}

const NO_HAND: HandShape = {
    layout: { rows: { rows: 1, rowN: [0, 0], cardW: WEB_HAND.cardWMax, height: WEB_HAND.cardH + WEB_HAND.rowPad }, slot: [] },
    lift: 0,
    planeRef: () => {},
};

const HandShapeContext = createContext<HandShape>(NO_HAND);

/** The hand's shape, for the hand and for what has to move with it. */
export const useHandShape = (): HandShape => useContext(HandShapeContext);

/** Owns the hand's measured width and its layout, for an interactive board. A
 *  board with no hand of mine (a spectator, the replay) lifts nothing. */
export const HandShapeProvider = ({ children }: { children: React.ReactNode }) => {
    const { view: game, localHandOrder } = useServer();
    const { isAnimating, currentAnimation } = useAnimation();
    const [plane, setPlane] = useState<HTMLDivElement | null>(null);
    const [width, setWidth] = useState(() => handWidth(null));

    useLayoutEffect(() => {
        setWidth(handWidth(plane));
        if (!plane || typeof ResizeObserver === 'undefined') return;
        const ro = new ResizeObserver(() => setWidth(handWidth(plane)));
        ro.observe(plane);
        return () => ro.disconnect();
    }, [plane]);

    const mine = !!game && game.mySeat >= 0;
    const arriving = mine && isAnimating && currentAnimation && currentAnimation.to_location === 'hand'
        && currentAnimation.seat === game!.mySeat ? (currentAnimation.cards ?? []) : [];
    const count = mine ? handLaidOut(localHandOrder, arriving).length : 0;

    const planeRef = useCallback((el: HTMLDivElement | null) => setPlane(el), []);
    const shape = useMemo<HandShape>(() => {
        if (!mine) return NO_HAND;
        const layout = webHandLayout(count, width);
        return { layout, lift: layout.rows.height - webHandLayout(0, width).rows.height, planeRef };
    }, [mine, count, width, planeRef]);

    return <HandShapeContext.Provider value={shape}>{children}</HandShapeContext.Provider>;
};
