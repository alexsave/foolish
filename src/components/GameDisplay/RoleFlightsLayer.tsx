// RoleFlightsLayer - the two marks that TRAVEL, drawn while they are in the air.
//
// THIS IS A PORT, NOT A DESIGN: `RoleFlightsLayer` and `runRoleFlights` from
// ios/FoolishKit/Boards/FRoleMotion.swift and
// ios/FoolishKit/Boards/MessageTableView+Roles.swift. The curve, the spin, the
// lift and the shadow are all src/state/roleMotion.ts's, and the Swift carries
// the reasoning.
//
// ABOVE THE BOARD AND ABOVE THE CARDS: the hand-off is the thing being read at
// that moment, and a shield passing behind a badge would read as a glitch. The
// seat a mark LEFT is blank for the whole flight and the seat it lands on turns
// its own mark away as it arrives (RoleCoin), so there is exactly one of each
// mark on screen at every instant of the hand-off.
//
// Viewport coordinates, like every other flight on this board
// (AnimationOverlay's `centreOf`): the ghost is hung by its CENTRE, so both
// endpoints are element centres and `translate(-50%, -50%)` does the hanging.

import { useEffect, useRef, useState } from 'react';
import { RoleMarkView } from '../RoleMark';
import {
    ROLE_FLIGHT_ARM_MS, ROLE_FLIGHT_MS, ROLE_FRAME_CAP_MS, flightLift, flightPointAt,
    flightScale, roleFlightEase, type RoleFlight,
} from '../../state/roleMotion';

/** One mark in the air. Its own frame loop, because the path is a quadratic
 *  bezier with a scale that peaks in the middle - neither of which a CSS
 *  transition can be handed - and because the frames that DRAW it are the only
 *  honest clock for it: a gap between two drawn frames is worth at most one
 *  frame at 60Hz, so a stalled board STRETCHES the flight instead of eating it
 *  (ROLE_FRAME_CAP_MS, and see RoleCoin for the same clock and the owner's "NO
 *  JUMPS IN ROTATION!"). */
const Ghost = ({ flight, onDrawn, onLanded }: {
    flight: RoleFlight;
    onDrawn: () => void;
    onLanded: () => void;
}) => {
    const ref = useRef<HTMLDivElement | null>(null);
    const [visible, setVisible] = useState(true);
    // Read through refs so a re-render of the layer cannot restart the flight.
    const cb = useRef({ onDrawn, onLanded });
    cb.current = { onDrawn, onLanded };

    useEffect(() => {
        let handle = 0;
        let last: number | null = null;
        let elapsed = 0;
        const write = (p: number) => {
            const el = ref.current;
            if (!el) return;
            const at = flightPointAt(flight, p);
            const lift = flightLift(p);
            el.style.left = `${at.x}px`;
            el.style.top = `${at.y}px`;
            el.style.transform =
                `translate(-50%, -50%) rotate(${flight.spin * p}deg) scale(${flightScale(p)})`;
            // iMessage's `.shadow(color: .black.opacity(0.45 * lift), radius: 8 *
            // lift, y: 6 * lift)`, read as CSS: the same three numbers, with the
            // radius as the blur.
            el.style.filter = `drop-shadow(0 ${6 * lift}px ${8 * lift}px rgba(0,0,0,${0.45 * lift}))`;
        };
        const step = (t: number) => {
            if (last !== null) elapsed += Math.min(Math.max(0, t - last), ROLE_FRAME_CAP_MS);
            last = t;
            cb.current.onDrawn();
            // ONE PAINT AT THE TAKE-OFF PAD before the tween starts - the same
            // beat a card gets (FlightCard.FLIGHT_ARM_MS), and for the same
            // reason: an animation that starts in the frame its view is created
            // in has nothing to interpolate from.
            const since = elapsed - ROLE_FLIGHT_ARM_MS;
            const p = since <= 0 ? 0 : Math.min(1, since / ROLE_FLIGHT_MS);
            write(roleFlightEase(p));
            if (p >= 1) {
                // The ghost is taken away the instant it lands, and the seat it
                // landed on stands the real mark up in that same frame. A whole
                // turn is the only spin that makes that hand-over seamless,
                // because 360 and 0 are the same angle.
                setVisible(false);
                cb.current.onLanded();
                return;
            }
            handle = requestAnimationFrame(step);
        };
        write(0);
        handle = requestAnimationFrame(step);
        return () => cancelAnimationFrame(handle);
    }, [flight]);

    if (!visible) return null;
    return (
        <div
            ref={ref}
            data-role-flight={flight.id}
            style={{
                position: 'absolute',
                left: `${flight.from.x}px`,
                top: `${flight.from.y}px`,
                transform: 'translate(-50%, -50%)',
                lineHeight: 0,
                willChange: 'transform, left, top',
            }}
        >
            <RoleMarkView kind={flight.kind} />
        </div>
    );
};

export const RoleFlightsLayer = ({ flights, onDrawn, onLanded }: {
    flights: readonly RoleFlight[];
    /** A ghost drew a frame. The hand-off's watchdog uses it to tell "still
     *  flying" from "never drawn at all". */
    onDrawn: () => void;
    /** EVERY ghost has landed, which is what ends the hand-off: the seats it
     *  emptied get their marks back on the frame the last ghost is taken away
     *  in. Driven by the ghosts and not by a wall-clock timer, so a board that
     *  stalls mid-flight finishes the flight rather than losing it. */
    onLanded: () => void;
}) => {
    const landed = useRef(0);
    const key = flights.map((f) => f.id).join(' ');
    useEffect(() => { landed.current = 0; }, [key]);
    if (flights.length === 0) return null;
    const one = () => {
        landed.current += 1;
        if (landed.current >= flights.length) onLanded();
    };
    return (
        <div
            style={{
                position: 'fixed',
                top: 0,
                left: 0,
                width: '100%',
                height: '100%',
                pointerEvents: 'none',
                // Above the card flights: while a mark is being handed over it is
                // the thing being read.
                zIndex: 10001,
                userSelect: 'none',
            }}
        >
            {flights.map((f) => <Ghost key={f.id} flight={f} onDrawn={onDrawn} onLanded={one} />)}
        </div>
    );
};
