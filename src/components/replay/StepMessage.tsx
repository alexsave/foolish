// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import React from 'react';
import type { ViewCard as Card } from '../../state/view';
import { Text } from '../Text';
import { SovietIcon } from '../SovietIcon';
import { ReplayFrame, REPLAY_STEP } from '../../replay/frames';
import { InlineCard } from './InlineCards';
import { seatName } from './seatName';

/* What just happened, in the viewer's language. The kind comes from the kernel
 * (frames.ts) rather than from the frame's events, because on the wire an attack
 * and a pass are one event type told apart only by a reconstructed English
 * sentence - and this line is localized, so that sentence is no use here anyway.
 *
 * A step is one ACTION, and its frame carries everything that action caused, so
 * a cover that ends a bout narrates as the cover; the discard and refills it
 * triggered animate under it rather than each claiming a line of their own. */
const StepMessage = ({ frame, names }: {
    frame: ReplayFrame; names: string[] | null;
}) => {
    const cards = (cs: Card[]) => (
        <span style={{ display: 'inline-flex', gap: 3, verticalAlign: 'middle' }}>
            {cs.map((c, i) => (
                <InlineCard key={i} card={c} />
            ))}
        </span>
    );
    const who = frame.seat !== null ? <b>{seatName(frame.seat, names)}</b> : null;

    switch (frame.kind) {
        case REPLAY_STEP.DEAL:
            return (
                <span>
                    <Text id="trump" />: {frame.cards.length > 0 && cards(frame.cards)}
                </span>
            );
        // THE replay_* KEYS, NOT THE BUTTON ONES. This line narrates what a
        // named player DID - "Alice attacks: 7of spades" - while `attack`,
        // `cover`, `pass`, `pickup` and `good` label the buttons the player
        // presses to do it. One key cannot be both: Russian shipped the button
        // wording ("Подкидываю", first person), so this line read "Alice I
        // throw in". Two meanings, two keys (c/i18n/keys.h).
        case REPLAY_STEP.ATTACK:
            return (
                <span>
                    {who} <SovietIcon name="sword" size={13} /> <Text id="replay_attack" />: {cards(frame.cards)}
                </span>
            );
        case REPLAY_STEP.COVER:
            return (
                <span>
                    {who} <Text id="replay_cover" />: {cards(frame.cards)} → {frame.target && cards([frame.target])}
                </span>
            );
        case REPLAY_STEP.PASS:
            return (
                <span>
                    {who} <Text id="replay_pass" />: {cards(frame.cards)}
                </span>
            );
        case REPLAY_STEP.PICKUP:
            return (
                <span>
                    {who} <Text id="replay_pickup" /> ({frame.count})
                </span>
            );
        case REPLAY_STEP.GOOD:
            return (
                <span>
                    {who} ✓ <Text id="replay_good" />
                </span>
            );
        // The bout closed: everyone still in said good, and the table went to
        // the discard. One step, because it is one thing that happened.
        case REPLAY_STEP.ROUND_END:
            return (
                <span>
                    ✓ <Text id="replay_good" /> - {frame.count} <Text id="discarded" />
                </span>
            );
        default:
            return null;
    }
};

/* The closing line: who was left holding cards. The board carries the 🃏 on the
 * fool's name; this says it in words, on the last step only. */
const FoolMessage = ({ fool, names }: { fool: number | null; names: string[] | null }) =>
    fool === null ? null : (
        <span>
            🃏 <b>{seatName(fool, names)}</b> <Text id="is_the_fool" />
        </span>
    );

export { StepMessage, FoolMessage };
