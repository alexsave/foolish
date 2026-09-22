// useRoleMotion.ts - the role marks, and the hand-off that moves them.
//
// WHAT THIS OWNS: the ledger of what the badges are WEARING, the landing pads
// each seat publishes, the two marks in the air at any moment, and the seats
// they left and are going to. It is the web's `MessageTableView+Roles.swift`.
//
// WHAT IT DOES NOT: which mark a seat wears and which marks travel, both pure
// and in src/state/roleLedger.ts; how a mark moves, pure and in
// src/state/roleMotion.ts; and WHEN each change is fired, which is the kernel's
// (anim_goods_opening / anim_goods_cleared / anim_pass_hand_off) and is asked
// from src/contexts/AnimationContext.tsx, where the sequence is.
//
// THE ONE RULE THAT IS NOT ITS OWN: who may write the ledger. A bystander - the
// board changing under a sequence that is still walking the marks forward - may
// not, and that is `anim_shown_ledger_allows`, asked and not restated.

import { useCallback, useEffect, useRef, useState } from 'react';
import { ANIM_CLAIM_BYSTANDER, ANIM_CLAIM_HAND_OFF, animShownLedgerAllows } from '@sdk/ts/wasm/bots.ts';
import {
    roleFlightsBetween, sameShownBoard, shownBoardOf,
    type ShownBoard, type ShownRoles,
} from './roleLedger';
import { ROLE_FLIGHT_ARM_MS, ROLE_FLIGHT_MS, type RoleFlight, type Spot } from './roleMotion';
import { GAME_STATUS, type TableView } from './view';

/** WHO IS WRITING (anim_plan.h ANIM_CLAIM_*). A hand-off is the one claim that
 *  is not a sequence and is still allowed to write while one runs: it is the
 *  only writer that reads what the badges are wearing, works out which marks
 *  changed hands and FLIES them. */
export const ROLE_CLAIM = { handOff: ANIM_CLAIM_HAND_OFF, bystander: ANIM_CLAIM_BYSTANDER } as const;

/** THE WHOLE HAND-OFF, IN ONE VALUE: the ghosts that carry the marks, and the
 *  seats they leave and land on.
 *
 *  These three cannot be split across two updates in either order. Blanking the
 *  departing seat first leaves the board with NO shield on it for a paint (the
 *  owner, on a frame he caught at the release: "THERE SHOULD ALWAYS BE EXACTLY
 *  ONE SHIELD"); marking the seats after the roles have published lets the
 *  receiving badge flip to the mark that is still in the air (two shields). */
export interface RoleHandOff {
    flights: readonly RoleFlight[];
    departing: ReadonlySet<number>;
    arriving: ReadonlySet<number>;
}

const NO_HAND_OFF: RoleHandOff = { flights: [], departing: new Set(), arriving: new Set() };

export interface RoleMotion {
    /** What the badges are wearing right now, or null for a board that has not
     *  shown anything yet (the one case the kernel is never asked about). The
     *  value a screen RENDERS from. */
    shown: ShownBoard | null;
    /** The same ledger, read from inside the frame loop. Two role answers can be
     *  fired in one frame - a good cleared by the very throw-in that passed the
     *  shield - and the second has to see what the first wrote, which a value
     *  captured by a closure a frame ago cannot. */
    read: () => ShownBoard | null;
    handOff: RoleHandOff;
    /** A seat publishes the box its mark is drawn in, so a shield can fly from
     *  an opponent to me and back. Call with null when the seat unmounts. */
    publishPad: (seat: number, el: HTMLElement | null) => void;
    /** Hand the roles over to `target`, flying whatever actually moved. Returns
     *  true when a mark took off. */
    syncRoles: (target: ShownRoles, opts?: { tableOpen?: boolean; animated?: boolean }) => boolean;
    /** The whole board's marks, from a view: the closing beat of a sequence, and
     *  a board that changed with no sequence at all. */
    syncFromView: (view: TableView | undefined, claim: number, sequencing: boolean) => boolean;
}

export function useRoleMotion(): RoleMotion {
    const [shown, setShown] = useState<ShownBoard | null>(null);
    const [handOff, setHandOff] = useState<RoleHandOff>(NO_HAND_OFF);
    // The ledger is read inside callbacks that run from the frame loop, so it is
    // held in a ref as well: two role answers can be fired in one frame (a good
    // cleared by the same throw-in that passed the shield) and the second must
    // see what the first wrote.
    const shownRef = useRef<ShownBoard | null>(null);
    const padsRef = useRef<Map<number, HTMLElement>>(new Map());
    // Claims the hand-off in flight, so a sequence that is replaced cannot have
    // its ghosts taken down by the timer of the one before it.
    const tokenRef = useRef(0);
    const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

    useEffect(() => () => { if (timerRef.current !== null) clearTimeout(timerRef.current); }, []);

    const publishPad = useCallback((seat: number, el: HTMLElement | null) => {
        if (el) padsRef.current.set(seat, el);
        else padsRef.current.delete(seat);
    }, []);

    /** Every seat's pad, measured NOW. Viewport coordinates, the same space the
     *  card flights are drawn in (AnimationOverlay's `centreOf`). A pad with no
     *  box yet - a seat laid out on this very frame - is left out, and the mark
     *  it would have carried changes in place instead. */
    const padSpots = (): Map<number, Spot> => {
        const out = new Map<number, Spot>();
        for (const [seat, el] of padsRef.current) {
            const r = el.getBoundingClientRect();
            if (r.width === 0 && r.height === 0) continue;
            out.set(seat, { x: r.left + r.width / 2, y: r.top + r.height / 2 });
        }
        return out;
    };

    const syncRoles = useCallback((target: ShownRoles,
                                   opts?: { tableOpen?: boolean; animated?: boolean }): boolean => {
        const was = shownRef.current;
        const next: ShownBoard = { roles: target, tableOpen: opts?.tableOpen ?? was?.tableOpen ?? false };
        if (was && sameShownBoard(was, next)) return false;
        shownRef.current = next;
        setShown(next);

        // Only when something actually MOVED: a cold board with nothing to hand
        // over from flies nothing, and neither does a change that took no mark
        // off a seat it belonged to.
        if (!was || opts?.animated === false) return false;
        const flights = roleFlightsBetween(was.roles, target, padSpots());
        if (flights.length === 0) return false;

        // THE HAND-OFF BEGINS NOW, in the same update as the roles that sent the
        // mark flying - not a hop later, which would let the receiving badge
        // start an ordinary flip to the mark still in the air (two shields).
        tokenRef.current += 1;
        const mine = tokenRef.current;
        setHandOff({
            flights,
            departing: new Set(flights.map((f) => f.fromSeat)),
            arriving: new Set(flights.map((f) => f.toSeat)),
        });
        if (timerRef.current !== null) clearTimeout(timerRef.current);
        timerRef.current = setTimeout(() => {
            timerRef.current = null;
            if (mine !== tokenRef.current) return;
            setHandOff(NO_HAND_OFF);
        }, ROLE_FLIGHT_ARM_MS + ROLE_FLIGHT_MS);
        return true;
    }, []);

    const syncFromView = useCallback((view: TableView | undefined, claim: number,
                                      sequencing: boolean): boolean => {
        if (!view || view.seats.length === 0) return false;
        if (!animShownLedgerAllows(claim, sequencing)) return false;
        const board = shownBoardOf(view);
        // A game that has ended hands nothing over: the board is about to give
        // way to the results, and a shield crossing it would be the last thing
        // on screen.
        const animated = view.status !== GAME_STATUS.GAME_OVER;
        return syncRoles(board.roles, { tableOpen: board.tableOpen, animated });
    }, [syncRoles]);

    const read = useCallback(() => shownRef.current, []);
    return { shown, read, handOff, publishPad, syncRoles, syncFromView };
}
