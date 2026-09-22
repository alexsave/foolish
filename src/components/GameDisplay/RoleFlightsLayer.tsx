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
    ROLE_FLIGHT_ARM_MS, ROLE_FLIGHT_MS, flightLift, flightPointAt, flightScale,
    roleFlightEase, type RoleFlight,
} from '../../state/roleMotion';

/** One mark in the air. Its own frame loop, because the path is a quadratic
 *  bezier with a scale that peaks in the middle - neither of which a CSS
 *  transition can be handed - and because the frames that DRAW it are the only
 *  honest clock for it (see RoleCoin). */
const Ghost = ({ flight }: { flight: RoleFlight }) => {
    const ref = useRef<HTMLDivElement | null>(null);
    const [visible, setVisible] = useState(true);

    useEffect(() => {
        let handle = 0;
        let started: number | null = null;
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
            // ONE PAINT AT THE TAKE-OFF PAD before the tween starts - the same
            // beat a card gets (FlightCard.FLIGHT_ARM_MS), and for the same
            // reason: an animation that starts in the frame its view is created
            // in has nothing to interpolate from.
            if (started === null) started = t;
            const elapsed = t - started - ROLE_FLIGHT_ARM_MS;
            if (elapsed < 0) {
                write(0);
                handle = requestAnimationFrame(step);
                return;
            }
            const p = Math.min(1, elapsed / ROLE_FLIGHT_MS);
            write(roleFlightEase(p));
            if (p >= 1) {
                // The ghost is taken away the instant it lands, and the seat it
                // landed on stands the real mark up in that same frame. A whole
                // turn is the only spin that makes that hand-over seamless,
                // because 360 and 0 are the same angle.
                setVisible(false);
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

export const RoleFlightsLayer = ({ flights }: { flights: readonly RoleFlight[] }) => {
    if (flights.length === 0) return null;
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
            {flights.map((f) => <Ghost key={f.id} flight={f} />)}
        </div>
    );
};
