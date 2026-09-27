// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import React, { useCallback, useEffect, useMemo, useState } from 'react';
import type { ViewCard as Card } from '../../state/view';
import { useServer } from '../../contexts/ServerContext';
import { animHandLaidOut } from '@sdk/ts/wasm/bots.ts';
import { ReplayGameState } from '../../replay/frames';
import { CardFace } from '../GameDisplay/CardFace';
import { InlineCard, InlineCardBack, ScaledCard } from './InlineCards';

/**
 * Reveal-hands overlay: every player's current hand face-up, positioned on
 * the same ellipse as PlayerRing (with the same viewer rotation: the replay
 * viewer is never a player, so self_index is -1 there and here). Cards are
 * the real CardFace/CardBack rendered at native 50×70 and scaled to 80%, so
 * they keep full card proportions - corner indices and center pip - instead
 * of the squished thin layout. Two centered rows per hand. Identities come
 * from replay_hands - retroactive knowledge of every card that ever
 * surfaces; cards that never get played stay face-down.
 *
 * Every hand here is drag-to-rearrangeable: the viewer can reorder the cards
 * within ANY player's hand exactly like reordering their own hand in the live
 * game. This is purely cosmetic and entirely client-side - there is no server
 * in a replay, so nothing is committed anywhere; we only keep a per-seat
 * "prefer local order" overlay (localOrders) that the render prefers, falling
 * back to the underlying replay_hands order and reconciling against the
 * current hand as the replay is scrubbed. THE RECONCILIATION IS THE KERNEL'S
 * (anim_plan.h anim_hand_laid_out_masked, through animHandLaidOut): the web
 * used to carry three implementations of "what order is a hand drawn in" - this
 * screen's, which reconciled face-down slots by count, and the live game's two,
 * which could not hold a face-down slot at all - so one rearrangement could come
 * out two different ways depending on which screen was drawing it. Cards are
 * NOT selectable or playable on the replay screen - only reordering.
 */
const RevealedHands = () => {
    const game = useServer().view as ReplayGameState | null;

    // Prefer-local-order overlay, keyed by seat index. Each entry is the
    // viewer's preferred ordering of that seat's hand; the render reconciles it
    // against the seat's current replay_hands order on every step/seek.
    const [localOrders, setLocalOrders] = useState<{ [seat: number]: (Card | null)[] }>({});

    // Active drag (STATE, not a ref, so the held card can render faded in place
    // exactly like the live hand): which seat's hand and which displayed slot is
    // being dragged. Reorder-only - no selection, no play, no cross-seat moves.
    const [drag, setDrag] = useState<{ seat: number; index: number } | null>(null);

    const displayHands = useMemo(() => {
        if (!game || !game.replay_hands) return null;
        return game.replay_hands.map((hand, index) =>
            animHandLaidOut(hand, localOrders[index] ?? []),
        );
    }, [game, localOrders]);

    // While a card is held, hovering another slot in the SAME seat swaps the two
    // - the live hand's real-time swap-on-hover (elementsFromPoint + data-*
    // indices). elementsFromPoint finds the slot under the cursor in EITHER
    // wrapped row, so dragging across rows just works; the seat filter keeps a
    // drag confined to one player's hand. The effect re-subscribes on every swap
    // so it always reads the latest dragged slot (mirrors the live DragContext).
    useEffect(() => {
        if (!drag) return;
        const onMove = (e: PointerEvent) => {
            const target = document.elementsFromPoint(e.clientX, e.clientY).find(
                (el) =>
                    el.getAttribute('data-replay-seat') === String(drag.seat) &&
                    el.getAttribute('data-replay-card-index') !== null,
            );
            if (!target) return;
            const to = parseInt(target.getAttribute('data-replay-card-index')!, 10);
            if (to === drag.index) return;
            setLocalOrders((prev) => {
                const base = [...(prev[drag.seat] ?? displayHands?.[drag.seat] ?? [])];
                const tmp = base[drag.index];
                base[drag.index] = base[to];
                base[to] = tmp;
                return { ...prev, [drag.seat]: base };
            });
            setDrag({ seat: drag.seat, index: to });
        };
        const onUp = () => setDrag(null);
        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
        document.addEventListener('pointercancel', onUp);
        return () => {
            document.removeEventListener('pointermove', onMove);
            document.removeEventListener('pointerup', onUp);
            document.removeEventListener('pointercancel', onUp);
        };
    }, [drag, displayHands]);

    const startDrag = useCallback(
        (seat: number, index: number, hand: (Card | null)[]) => (e: React.PointerEvent) => {
            e.preventDefault();
            e.stopPropagation();
            // Seed the seat's preferred order from what's currently shown so the
            // first swap reorders the exact cards on screen.
            setLocalOrders((prev) => (prev[seat] ? prev : { ...prev, [seat]: hand }));
            setDrag({ seat, index });
        },
        [],
    );

    if (!game || !game.replay_hands || !displayHands) return null;
    const n = game.seats.length;

    return (
        <>
            {displayHands.map((hand, index) => {
                if (hand.length === 0) return null;
                const visual_index = (index + 1) % n; // self_index = -1, as in PlayerRing
                const radians = (2 * Math.PI * visual_index) / n;
                const x = (-1 * Math.sin(radians) * 35) + 50 + '%';
                const y = (Math.cos(radians) * 35) + 50 + '%';
                return (
                    <div
                        key={index}
                        style={{
                            position: 'absolute',
                            top: y,
                            left: x,
                            transform: 'translate(-50%, 32px)',
                            display: 'flex',
                            flexWrap: 'wrap',
                            alignItems: 'center',
                            justifyContent: 'center',
                            gap: 3,
                            // full-proportion cards, wrapping into two rows
                            width: Math.ceil(hand.length / 2) * 43 + 6,
                            zIndex: 60,
                            // the wrapper stays inert; only the draggable card
                            // slots below re-enable pointer events
                            pointerEvents: 'none',
                        }}
                    >
                        {hand.map((c, i) => {
                            const isDragged = drag?.seat === index && drag?.index === i;
                            return (
                                <div
                                    // stable per-card key (like the live hand's
                                    // value+suit key) so React MOVES the node on a
                                    // swap instead of repainting content in place -
                                    // that's what makes the reorder read cleanly.
                                    key={c ? `${c.suit}-${c.value}` : `back-${i}`}
                                    data-replay-seat={index}
                                    data-replay-card-index={i}
                                    onPointerDown={startDrag(index, i, hand)}
                                    style={{
                                        display: 'inline-flex',
                                        pointerEvents: 'auto',
                                        cursor: isDragged ? 'grabbing' : 'grab',
                                        touchAction: 'none',
                                        // match the live hand: the held card stays
                                        // in flow as a faded placeholder while the
                                        // rest swap around it.
                                        opacity: isDragged ? 0.3 : 1,
                                        transition: 'opacity 0.1s ease',
                                    }}
                                >
                                    {c ? (
                                        <InlineCard card={c} w={40} />
                                    ) : (
                                        <InlineCardBack w={40} />
                                    )}
                                </div>
                            );
                        })}
                    </div>
                );
            })}
        </>
    );
};

export { RevealedHands };
