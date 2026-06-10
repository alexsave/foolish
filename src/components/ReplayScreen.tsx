import React, { useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { Card } from '../common/types';
import { LOG_TYPE } from '../common/types';
import { HEARTS, DIAMONDS } from '../common/constants';
import { Text } from './Text';
import { SovietIcon } from './SovietIcon';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { codeToGame } from '../replay/codec';
import { decodeReplay } from '../replay/decode';
import { DecodedReplay } from '../replay/core';
import { buildReplaySteps, ReplayStep } from '../replay/view';

/**
 * Self-contained replay viewer: WWW.FOOLISH.CARDS/<base32> — the path segment
 * IS the entire game (decoded client-side, no auth, no database row). Renders
 * one snapshot per game event with step/scrub/autoplay controls.
 *
 * Deliberately context-light: the in-game card components depend on the
 * animation/server providers mounted inside ProtectedRoute, so this screen
 * brings its own tiny card renderers and works for signed-out visitors.
 */

const SUIT_GLYPHS = ['♠', '♥', '♣', '♦'];
const VALUE_GLYPHS = ['', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A'];

const suitColor = (suit: number) =>
    suit === HEARTS || suit === DIAMONDS ? '#dc2626' : '#1a1a1a';

const MiniCard = ({ card, trumpSuit, w = 30 }: { card: Card; trumpSuit?: number; w?: number }) => {
    const h = Math.round(w * 1.4);
    return (
        <div
            style={{
                width: w,
                height: h,
                borderRadius: Math.max(3, w * 0.12),
                background: '#faf7ef',
                border: card.suit === trumpSuit ? '1.5px solid #d4a017' : '1.5px solid #999',
                color: suitColor(card.suit),
                display: 'inline-flex',
                flexDirection: 'column',
                alignItems: 'center',
                justifyContent: 'center',
                fontFamily: 'Georgia, serif',
                fontWeight: 'bold',
                fontSize: w * 0.42,
                lineHeight: 1.05,
                boxShadow: '0 1px 2px rgba(0,0,0,0.35)',
                userSelect: 'none',
                flexShrink: 0,
            }}
        >
            <div>{VALUE_GLYPHS[card.value] ?? '?'}</div>
            <div>{SUIT_GLYPHS[card.suit] ?? '?'}</div>
        </div>
    );
};

const MiniBack = ({ count, w = 30 }: { count: number; w?: number }) => {
    const h = Math.round(w * 1.4);
    if (count <= 0) return null;
    return (
        <div style={{ position: 'relative', display: 'inline-block', flexShrink: 0 }}>
            <div
                style={{
                    width: w,
                    height: h,
                    borderRadius: Math.max(3, w * 0.12),
                    background: '#B32929',
                    border: '1.5px solid #7a1d1d',
                    boxShadow: '0 1px 2px rgba(0,0,0,0.35)',
                }}
            />
            <div
                style={{
                    position: 'absolute',
                    inset: 0,
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    color: '#F5E6C8',
                    fontWeight: 'bold',
                    fontSize: w * 0.45,
                    textShadow: '0 1px 2px rgba(0,0,0,0.6)',
                }}
            >
                {count}
            </div>
        </div>
    );
};

const seatName = (seat: number) => `P${seat + 1}`;

const StepMessage = ({ step }: { step: ReplayStep }) => {
    const cards = (cs: Card[], trump?: number) => (
        <span style={{ display: 'inline-flex', gap: 3, verticalAlign: 'middle' }}>
            {cs.map((c, i) => (
                <MiniCard key={i} card={c} trumpSuit={trump} w={22} />
            ))}
        </span>
    );
    const who = step.seat !== null ? <b>{seatName(step.seat)}</b> : null;

    switch (step.kind) {
        case LOG_TYPE.GAME_START:
            return (
                <span>
                    <Text id="trump" />: {step.flipped && cards([step.flipped])}
                </span>
            );
        case LOG_TYPE.ATTACK:
            return (
                <span>
                    {who} <SovietIcon name="sword" size={14} /> <Text id="attack" />: {cards(step.cards)}
                </span>
            );
        case LOG_TYPE.COVER:
            return (
                <span>
                    {who} <Text id="cover" />: {cards(step.cards)} → {step.target && cards([step.target])}
                </span>
            );
        case LOG_TYPE.PASS:
            return (
                <span>
                    {who} <Text id="pass" />: {cards(step.cards)}
                </span>
            );
        case LOG_TYPE.PICKUP:
            return (
                <span>
                    {who} <Text id="pickup" /> ({step.count})
                </span>
            );
        case LOG_TYPE.GOOD:
            return (
                <span>
                    {who} ✓ <Text id="good" />
                </span>
            );
        case LOG_TYPE.DRAW:
            return (
                <span>
                    {who} <Text id="draws" /> {step.count > 0 && <>+{step.count}</>}{' '}
                    {step.cards.length > 0 && cards(step.cards)}
                </span>
            );
        case LOG_TYPE.DISCARD:
            return (
                <span>
                    {step.count} <Text id="discarded" />
                </span>
            );
        case LOG_TYPE.PLAYER_OUT:
            return (
                <span>
                    {who} <Text id="is_out" />
                </span>
            );
        case LOG_TYPE.DEFENDER_CHANGE: {
            const def = step.players.findIndex((p) => p.isDefender);
            return (
                <span>
                    <SovietIcon name="shield" size={14} /> <Text id="defender" />:{' '}
                    <b>{def >= 0 ? seatName(def) : '—'}</b>
                </span>
            );
        }
        case 'end':
            return (
                <span>
                    🎉 {who} <Text id="is_the_fool" />
                </span>
            );
        default:
            return null;
    }
};

const PlayerBox = ({
    seat,
    view,
    isActor,
    isFool,
    trumpSuit,
}: {
    seat: number;
    view: ReplayStep['players'][number];
    isActor: boolean;
    isFool: boolean;
    trumpSuit: number;
}) => (
    <div
        style={{
            display: 'flex',
            flexDirection: 'column',
            gap: 4,
            padding: '6px 8px',
            borderRadius: 8,
            background: isActor ? 'rgba(231,151,67,0.25)' : 'rgba(0,0,0,0.25)',
            outline: isActor ? '1.5px solid #E79743' : 'none',
            opacity: view.out && !isFool ? 0.45 : 1,
            minWidth: 76,
            maxWidth: 200,
        }}
    >
        <div
            className="text-shadow"
            style={{
                display: 'flex',
                alignItems: 'center',
                gap: 5,
                color: 'var(--color-text-primary)',
                fontWeight: 'bold',
                fontSize: '0.85rem',
            }}
        >
            {seatName(seat)}
            {view.isDefender && <SovietIcon name="shield" size={14} />}
            {view.good && <span style={{ fontSize: '0.75rem' }}>✓</span>}
            {isFool && <span>🃏</span>}
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 3, alignItems: 'center' }}>
            <MiniBack count={view.hidden} w={26} />
            {view.known.map((c, i) => (
                <MiniCard key={i} card={c} trumpSuit={trumpSuit} w={26} />
            ))}
        </div>
    </div>
);

export const ReplayScreen = ({ code }: { code: string }) => {
    const result = useMemo<{ decoded: DecodedReplay; steps: ReplayStep[] } | null>(() => {
        try {
            const decoded = decodeReplay(codeToGame(code));
            return { decoded, steps: buildReplaySteps(decoded) };
        } catch (e) {
            console.error('Replay decode failed:', e);
            return null;
        }
    }, [code]);

    const [stepIdx, setStepIdx] = useState(0);
    const [playing, setPlaying] = useState(false);
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;

    const lastIdx = result ? result.steps.length - 1 : 0;

    useEffect(() => {
        if (!playing || !result) return;
        const t = setInterval(() => {
            if (stepRef.current >= lastIdx) {
                setPlaying(false);
            } else {
                setStepIdx((i) => Math.min(i + 1, lastIdx));
            }
        }, 650);
        return () => clearInterval(t);
    }, [playing, lastIdx, result]);

    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'ArrowRight') setStepIdx((i) => Math.min(i + 1, lastIdx));
            if (e.key === 'ArrowLeft') setStepIdx((i) => Math.max(i - 1, 0));
            if (e.key === ' ') {
                e.preventDefault();
                setPlaying((p) => !p);
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [lastIdx]);

    if (!result) {
        return (
            <div className="page" style={{ padding: '2rem', textAlign: 'center' }}>
                <WoolBackgroundLayer />
                <h2 className="text-shadow" style={{ color: 'var(--color-text-primary)' }}>
                    <Text id="invalid_replay" />
                </h2>
                <Link href="/" className="text-shadow" style={{ color: 'var(--color-text-primary)' }}>
                    <Text id="back_to_home" />
                </Link>
            </div>
        );
    }

    const { decoded, steps } = result;
    const step = steps[stepIdx];
    const trumpSuit = decoded.powerSuit;
    const atEnd = step.kind === 'end';

    const btnStyle: React.CSSProperties = {
        minWidth: 44,
        padding: '0.45rem 0.6rem',
        fontSize: '1rem',
        cursor: 'pointer',
    };

    return (
        <div
            className="page"
            style={{
                padding: '0.75rem',
                display: 'flex',
                flexDirection: 'column',
                gap: '0.7rem',
                alignItems: 'center',
                width: '100%',
                overflowY: 'auto',
            }}
        >
            <WoolBackgroundLayer />

            {/* header: title, trump, stock, discard */}
            <div
                style={{
                    display: 'flex',
                    alignItems: 'center',
                    gap: '0.9rem',
                    flexWrap: 'wrap',
                    justifyContent: 'center',
                }}
            >
                <h2 className="text-shadow" style={{ margin: 0, color: 'var(--color-text-primary)' }}>
                    <Text id="replay_title" />
                </h2>
                <span
                    className="text-shadow"
                    style={{ display: 'inline-flex', alignItems: 'center', gap: 5, color: 'var(--color-text-primary)', fontSize: '0.85rem' }}
                >
                    <Text id="trump" />
                    <MiniCard card={decoded.trumpCard} trumpSuit={trumpSuit} w={26} />
                </span>
                <span
                    className="text-shadow"
                    style={{ display: 'inline-flex', alignItems: 'center', gap: 5, color: 'var(--color-text-primary)', fontSize: '0.85rem' }}
                >
                    <MiniBack count={step.deckCount + (step.flipped ? 1 : 0)} w={26} />
                    <Text id="deck_cards" />
                </span>
                <span
                    className="text-shadow"
                    style={{ color: 'var(--color-text-primary)', fontSize: '0.85rem' }}
                >
                    🗑 {step.discard}
                </span>
            </div>

            {/* players */}
            <div
                style={{
                    display: 'flex',
                    flexWrap: 'wrap',
                    gap: 8,
                    justifyContent: 'center',
                    width: '100%',
                    maxWidth: 900,
                }}
            >
                {step.players.map((p, s) => (
                    <PlayerBox
                        key={s}
                        seat={s}
                        view={p}
                        isActor={step.seat === s && !atEnd}
                        isFool={atEnd && decoded.fool === s}
                        trumpSuit={trumpSuit}
                    />
                ))}
            </div>

            {/* table battles */}
            <div
                style={{
                    display: 'flex',
                    flexWrap: 'wrap',
                    gap: 14,
                    justifyContent: 'center',
                    alignItems: 'flex-start',
                    minHeight: 86,
                    padding: '0.4rem',
                }}
            >
                {step.battles.map((b, i) => (
                    <div key={i} style={{ position: 'relative', width: 44, height: 80 }}>
                        <div style={{ position: 'absolute', top: 0, left: 0 }}>
                            <MiniCard card={b.attack} trumpSuit={trumpSuit} w={40} />
                        </div>
                        {b.defense && (
                            <div
                                style={{
                                    position: 'absolute',
                                    top: 18,
                                    left: 8,
                                    transform: 'rotate(12deg)',
                                }}
                            >
                                <MiniCard card={b.defense} trumpSuit={trumpSuit} w={40} />
                            </div>
                        )}
                    </div>
                ))}
            </div>

            {/* current event */}
            <div
                className="text-shadow"
                style={{
                    color: 'var(--color-text-primary)',
                    fontSize: '0.95rem',
                    minHeight: 34,
                    display: 'flex',
                    alignItems: 'center',
                    gap: 6,
                }}
            >
                <StepMessage step={step} />
            </div>

            {/* controls */}
            <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                <TexturedSurface as="button" seed={0.31} className="btn-wood" style={btnStyle}
                    onClick={() => { setPlaying(false); setStepIdx(0); }}>
                    <span className="btn-wood-text">⏮</span>
                </TexturedSurface>
                <TexturedSurface as="button" seed={0.45} className="btn-wood" style={btnStyle}
                    onClick={() => { setPlaying(false); setStepIdx((i) => Math.max(i - 1, 0)); }}>
                    <span className="btn-wood-text">◀</span>
                </TexturedSurface>
                <TexturedSurface as="button" seed={0.58} className="btn-wood" style={btnStyle}
                    onClick={() => setPlaying((p) => !p)}>
                    <span className="btn-wood-text">{playing ? '⏸' : '▶'}</span>
                </TexturedSurface>
                <TexturedSurface as="button" seed={0.71} className="btn-wood" style={btnStyle}
                    onClick={() => { setPlaying(false); setStepIdx((i) => Math.min(i + 1, lastIdx)); }}>
                    <span className="btn-wood-text">▶▶</span>
                </TexturedSurface>
                <TexturedSurface as="button" seed={0.84} className="btn-wood" style={btnStyle}
                    onClick={() => { setPlaying(false); setStepIdx(lastIdx); }}>
                    <span className="btn-wood-text">⏭</span>
                </TexturedSurface>
            </div>

            <div style={{ display: 'flex', gap: 10, alignItems: 'center', width: '100%', maxWidth: 480 }}>
                <input
                    type="range"
                    min={0}
                    max={lastIdx}
                    value={stepIdx}
                    onChange={(e) => { setPlaying(false); setStepIdx(Number(e.target.value)); }}
                    style={{ flex: 1 }}
                />
                <span
                    className="text-shadow"
                    style={{ color: 'var(--color-text-primary)', fontSize: '0.8rem', whiteSpace: 'nowrap' }}
                >
                    {stepIdx + 1}/{steps.length}
                </span>
            </div>

            <Link
                href="/"
                className="text-shadow"
                style={{ color: 'var(--color-text-primary)', fontSize: '0.85rem', opacity: 0.85 }}
            >
                <Text id="back_to_home" />
            </Link>
        </div>
    );
};
