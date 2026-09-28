// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import React from 'react';

/* Flat VHS-deck transport glyphs - geometric, single-colour (currentColor),
 * no strokes or gradients. A "bar at the point" turns the plain play/rewind
 * triangle into a step glyph; doubled triangles are the bout-skip glyphs. */
const Glyph = ({ children }: { children: React.ReactNode }) => (
    <svg width={18} height={18} viewBox="0 0 24 24" fill="currentColor" aria-hidden>
        {children}
    </svg>
);
const IconStepBack = () => (
    <Glyph>
        <rect x={5} y={5} width={2.6} height={14} />
        <polygon points="20,5 20,19 9,12" />
    </Glyph>
);
const IconStepForward = () => (
    <Glyph>
        <polygon points="4,5 4,19 15,12" />
        <rect x={16.4} y={5} width={2.6} height={14} />
    </Glyph>
);
const IconBoutStart = () => (
    <Glyph>
        <rect x={2} y={5} width={2.4} height={14} />
        <polygon points="13,5 13,19 5.5,12" />
        <polygon points="21,5 21,19 13.5,12" />
    </Glyph>
);
const IconBoutNext = () => (
    <Glyph>
        <polygon points="3,5 3,19 10.5,12" />
        <polygon points="11,5 11,19 18.5,12" />
        <rect x={19.6} y={5} width={2.4} height={14} />
    </Glyph>
);
const IconPlay = () => (
    <Glyph>
        <polygon points="6,4 6,20 20,12" />
    </Glyph>
);
const IconPause = () => (
    <Glyph>
        <rect x={6} y={4} width={4} height={16} />
        <rect x={14} y={4} width={4} height={16} />
    </Glyph>
);
const IconEye = () => (
    <Glyph>
        <path d="M12 5C6.5 5 2.7 9.2 1.5 12c1.2 2.8 5 7 10.5 7s9.3-4.2 10.5-7C21.3 9.2 17.5 5 12 5Zm0 11a4 4 0 1 1 0-8 4 4 0 0 1 0 8Z" />
        <circle cx={12} cy={12} r={2} />
    </Glyph>
);
/* Telestrator pen - a simple diagonal marker; the active state tints the
   whole knob amber like the other transport toggles. */
const IconPen = () => (
    <Glyph>
        <path d="M16.5 3.5a2 2 0 0 1 2.8 2.8L8.7 16.9 4 18.5l1.6-4.7L16.5 3.5Z" />
    </Glyph>
);
/* Oracle - a crystal ball on its stand; active state tints the knob amber. */
const IconOracle = () => (
    <Glyph>
        <circle cx={12} cy={10} r={6} />
        <path d="M6.5 17.5h11L19 21H5l1.5-3.5Z" />
        <circle cx={9.7} cy={8} r={1.6} fill="#161618" />
    </Glyph>
);

export { Glyph, IconStepBack, IconStepForward, IconBoutStart, IconBoutNext,
         IconPlay, IconPause, IconEye, IconPen, IconOracle };
