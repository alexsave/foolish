import { PersonalGame } from "../../common/types";

export const DefenderShield = ({ state, self_index }: { state: PersonalGame, self_index: number }) => {
    return <>

        {/* Debug: Green arrow from shield center towards defender player (20px long) */}
        <svg style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 50 }}>
            {(() => {
                // Only show for defender
                const defenderPlayer = state.players[state.defender];
                if (!defenderPlayer) return null;

                const visual_index = (state.defender - self_index + state.players.length) % state.players.length;
                const radians = (2) * Math.PI * visual_index / (state.players.length);

                // Calculate shield position (same as debug dots)

                const H = window.innerHeight;
                const W = window.innerWidth;
                const aPct = 35;                          // ellipse semi-axis in %
                const cxPct = 50, cyPct = 50;             // centre in %

                // defender in %
                const dxPct = aPct * Math.cos(radians + Math.PI / 2);  // –sinθ
                const dyPct = aPct * Math.sin(radians + Math.PI / 2);  //  cosθ

                // length of that vector in px
                const dxPx = dxPct * W / 100;
                const dyPx = dyPct * H / 100;
                const rPx = Math.hypot(dxPx, dyPx);

                // step 60 px inward  (= keep direction, shorten length)
                const startScale = (rPx - 36) / rPx;
                const arrowStartX = cxPct + dxPct * startScale;
                const arrowStartY = cyPct + dyPct * startScale;

                const endScale = (rPx - 35) / rPx;
                const arrowEndX = cxPct + dxPct * endScale;
                const arrowEndY = cyPct + dyPct * endScale;

                return (
                    <line
                        key="debug-arrow-defender"
                        x1={`${arrowStartX}%`}
                        y1={`${arrowStartY}%`}
                        x2={`${arrowEndX}%`}
                        y2={`${arrowEndY}%`}
                        stroke="black"
                        strokeWidth="4"
                        markerEnd="url(#blackArrowHead)"
                    />
                );
            })()}

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
                    <polygon
                        points="0 0, 2 2, 0 4"
                        fill="black"
                    />
                </marker>
            </defs>
        </svg>

        {/* Shield and arrow pointing to defender */}
        {(() => {
            const defenderPlayer = state.players[state.defender];
            if (!defenderPlayer) return null;

            const visual_index = (state.defender - self_index + state.players.length) % state.players.length;

            // Calculate defender position
            const H = window.innerHeight;
            const W = window.innerWidth;
            const aPct = 35;                          // ellipse semi-axis in %
            const cxPct = 50, cyPct = 50;             // centre in %

            const radians = 2 * Math.PI * visual_index / state.players.length;

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

            return (
                <div style={{
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
                        fontSize: '24px',
                        zIndex: 400
                    }}>
                        🛡️
                    </div>
                </div>
            );
        })()}
    </>
};