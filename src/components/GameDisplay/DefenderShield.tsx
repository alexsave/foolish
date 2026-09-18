import { useServer } from "../../contexts/ServerContext";
import { SovietIcon } from "../SovietIcon";
import { rulesOf, type TableView } from "../../state/view";

export const DefenderShield = () => {
    const game = useServer().view as TableView;
    const self_index = game.mySeat;

    // The shield's seat is the kernel's (client_view_rules): the defender, and
    // no one during the deal's intermediate state (deck laid out, trump not yet turned).
    const defender = rulesOf(game).defenderBadge;
    if (defender < 0) {
        return <></>;
    }

    const visual_index = (defender - self_index + game.seats.length) % game.seats.length;
    const radians = (2) * Math.PI * visual_index / (game.seats.length);

    // Calculate defender position
    const H = window.innerHeight;
    const W = window.innerWidth;

    // ellipse semi-axis in %
    const aPct = 35;
    // centre in %
    const cxPct = 50, cyPct = 50;

    // defender in %
    const dxPct = aPct * Math.cos(radians + Math.PI / 2);  // –sinθ
    const dyPct = aPct * Math.sin(radians + Math.PI / 2);  //  cosθ

    // length of that vector in px
    const dxPx = dxPct * W / 100;
    const dyPx = dyPct * H / 100;
    const rPx = Math.hypot(dxPx, dyPx);

    // step 60 px inward  (= keep direction, shorten length)
    const scale = (rPx - 55) / rPx;
    const shieldXPct = cxPct + dxPct * scale;
    const shieldYPct = cyPct + dyPct * scale;

    // step 60 px inward  (= keep direction, shorten length)
    const startScale = (rPx - 36) / rPx;
    const arrowStartX = cxPct + dxPct * startScale;
    const arrowStartY = cyPct + dyPct * startScale;

    const endScale = (rPx - 35) / rPx;
    const arrowEndX = cxPct + dxPct * endScale;
    const arrowEndY = cyPct + dyPct * endScale;

    return <>

        {/* Keyed by defender so a defender change remounts the arrow instead of
            leaving a stale one on screen. The arrowhead is a marker with the
            default markerUnits="strokeWidth", so strokeWidth scales the whole
            triangle uniformly — bumped for size WITHOUT altering its angles. */}
        <svg key={`defender-arrow-${game.defender}`} style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 50 }}>

            <line
                x1={`${arrowStartX}%`}
                y1={`${arrowStartY}%`}
                x2={`${arrowEndX}%`}
                y2={`${arrowEndY}%`}
                stroke="black"
                strokeWidth="6"
                markerEnd="url(#blackArrowHead)"
            />
            {/* Arrow marker definition for black arrows */}
            <defs>
                <marker
                    id="blackArrowHead"
                    markerWidth="4"
                    markerHeight="4"
                    refX="1"
                    refY="2"
                    orient="auto"
                >
                    <polygon points="0 0, 2 2, 0 4" fill="black" />
                </marker>
            </defs>
        </svg>

        {/* Shield and arrow pointing to defender */}
        <div key={`defender-shield-${game.defender}`} style={{
            position: 'absolute',
            left: `${shieldXPct}%`,
            top: `${shieldYPct}%`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center'
        }}>

            {/* Shield */}
            <div style={{
                position: 'absolute',
                zIndex: 400
            }}>
                <SovietIcon name="shield" size={28} />
            </div>
        </div>
    </>
};