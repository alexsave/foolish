import React from 'react';

// Soviet card back — red field, gold border, gold star. Shared by CardBack
// (full-size card) and PlayerRing (mini stacked backs); the only difference is
// the extra positioning style the ring overlays pass in.
export const SovietCardBack = ({ style }: { style?: React.CSSProperties }) => (
    <svg width="100%" height="100%" viewBox="0 0 50 70" preserveAspectRatio="none" style={{ display: 'block', ...style }}>
        <rect x="0" y="0" width="50" height="70" fill="#B32929" />
        <rect x="2" y="2" width="46" height="66" fill="none" stroke="#E79743" strokeWidth="3" />
        <polygon
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29"
            fill="none"
            stroke="#0A0A0A"
            strokeWidth="3"
        />
        <polygon
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29"
            fill="none"
            stroke="#F5E6C8"
            strokeWidth="1.5"
        />
        <polygon
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29"
            fill="#E79743"
        />
    </svg>
);
