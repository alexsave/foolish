// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import React from 'react';
import type { ViewCard as Card } from '../../state/view';
import { CardFace } from '../GameDisplay/CardFace';
import { CardBack } from '../GameDisplay/CardBack';

/* Miniature cards rendered through the REAL CardFace/CardBack (native
 * 50×70 px, shrunk with a CSS transform) so they match the table exactly -
 * corner indices, center pip, theme styling, Soviet suit icons and all.
 * These render inside the replay's provider tree, which CardFace/CardBack
 * need (animation, styles, fern pattern). */
const CARD_W = 50;
const CARD_H = 70;

// Shared scaled-down wrapper: a CARD_W×CARD_H card rendered at `w` px wide via
// a CSS transform, so the real CardFace/CardBack render at native size.
const ScaledCard = ({ w = 22, children }: { w?: number; children: React.ReactNode }) => {
    const scale = w / CARD_W;
    return (
        <span
            style={{
                display: 'inline-block',
                width: w,
                height: Math.round(CARD_H * scale),
                position: 'relative',
                flexShrink: 0,
                verticalAlign: 'middle',
            }}
        >
            <span
                style={{
                    position: 'absolute',
                    top: 0,
                    left: 0,
                    transform: `scale(${scale})`,
                    transformOrigin: 'top left',
                    display: 'block',
                    width: CARD_W,
                    height: CARD_H,
                }}
            >
                {children}
            </span>
        </span>
    );
};

const InlineCard = ({ card, w = 22 }: { card: Card; w?: number }) => (
    <ScaledCard w={w}>
        <CardFace card={card} owner="replay-inline" />
    </ScaledCard>
);

const InlineCardBack = ({ w = 22 }: { w?: number }) => (
    <ScaledCard w={w}>
        <CardBack deckSize={1} />
    </ScaledCard>
);

export { CARD_W, CARD_H, ScaledCard, InlineCard, InlineCardBack };
