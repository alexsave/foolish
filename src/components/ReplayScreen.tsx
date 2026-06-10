import React, { useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { Card, LOG_TYPE } from '../common/types';
import { HEARTS, DIAMONDS } from '../common/constants';
import { Text } from './Text';
import { SovietIcon } from './SovietIcon';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { ReplayServerProvider } from '../contexts/ServerContext';
import { ReplayAnimationProvider } from '../contexts/AnimationContext';
import { GameProvider } from '../contexts/GameContext';
import { DragProvider } from '../contexts/DragContext';
import { FernFractalProvider } from '../utils/fernFractal';
import { TableBattles } from './GameDisplay/TableBattles';
import { PlayerRing } from './GameDisplay/PlayerRing';
import { DefenderShield } from './GameDisplay/DefenderShield';
import { DeckAndFlipped } from './GameDisplay/DeckAndFlipped';
import { DiscardPile } from './GameDisplay/DiscardPile';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { codeToGame } from '../replay/codec';
import { decodeReplay } from '../replay/decode';
import { DecodedReplay } from '../replay/core';
import { buildReplaySteps, stepToGame, ReplayStep } from '../replay/view';

/**
 * Self-contained replay viewer: WWW.FOOLISH.CARDS/<base32> — the path segment
 * IS the entire game (decoded client-side, no auth, no database row).
 *
 * Renders through the REAL game display components (PlayerRing, TableBattles,
 * DeckAndFlipped, DiscardPile, DefenderShield): each step synthesizes a
 * PersonalGame snapshot (view.ts stepToGame) and serves it through
 * ReplayServerProvider, so the board looks exactly like a live game. The
 * animation context is the inert replay variant — the live one subscribes to
 * the per-user realtime channel, which doesn't exist here.
 */

const SUIT_GLYPHS = ['♠', '♥', '♣', '♦'];
const VALUE_GLYPHS = ['', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A'];

const InlineCard = ({ card }: { card: Card }) => (
    <span
        style={{
            display: 'inline-flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            width: 22,
            height: 31,
            borderRadius: 3,
            background: '#faf7ef',
            border: '1px solid #999',
            color: card.suit === HEARTS || card.suit === DIAMONDS ? '#dc2626' : '#1a1a1a',
            fontFamily: 'Georgia, serif',
            fontWeight: 'bold',
            fontSize: 10,
            lineHeight: 1.05,
            boxShadow: '0 1px 2px rgba(0,0,0,0.35)',
            userSelect: 'none',
            flexShrink: 0,
            verticalAlign: 'middle',
        }}
    >
        <span>{VALUE_GLYPHS[card.value] ?? '?'}</span>
        <span>{SUIT_GLYPHS[card.suit] ?? '?'}</span>
    </span>
);

const seatName = (seat: number) => `P${seat + 1}`;

const StepMessage = ({ step }: { step: ReplayStep }) => {
    const cards = (cs: Card[]) => (
        <span style={{ display: 'inline-flex', gap: 3, verticalAlign: 'middle' }}>
            {cs.map((c, i) => (
                <InlineCard key={i} card={c} />
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
                    {who} <SovietIcon name="sword" size={13} /> <Text id="attack" />: {cards(step.cards)}
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
                    <SovietIcon name="shield" size={13} /> <Text id="defender" />:{' '}
                    <b>{def >= 0 ? seatName(def) : '—'}</b>
                </span>
            );
        }
        case 'end':
            return (
                <span>
                    🃏 {who} <Text id="is_the_fool" />
                </span>
            );
        default:
            return null;
    }
};

const ReplayBoard = ({ decoded, step }: { decoded: DecodedReplay; step: ReplayStep }) => {
    usePreventScroll();
    const game = useMemo(() => stepToGame(decoded, step), [decoded, step]);

    return (
        <ReplayServerProvider game={game}>
            <FernFractalProvider>
                <ReplayAnimationProvider>
                    <GameProvider>
                        <DragProvider>
                            <div className="flex flex-1 flex-center">
                                <p className="title--game-display">
                                    <Text id="replay_title" />
                                </p>

                                <DeckAndFlipped />
                                <DiscardPile />

                                <div
                                    className="absolute flex flex-col items-center justify-center w-full"
                                    style={{ top: 0, bottom: 0 }}
                                >
                                    <DefenderShield />
                                    <TableBattles />
                                </div>

                                <PlayerRing />
                            </div>
                        </DragProvider>
                    </GameProvider>
                </ReplayAnimationProvider>
            </FernFractalProvider>
        </ReplayServerProvider>
    );
};

export const ReplayScreen = ({ code }: { code: string }) => {
    // Client-only: the game display reads window dimensions during render
    // (DefenderShield), so skip SSR/prerender entirely.
    const [mounted, setMounted] = useState(false);
    useEffect(() => setMounted(true), []);

    const result = useMemo<{ decoded: DecodedReplay; steps: ReplayStep[] } | null>(() => {
        if (!mounted) return null;
        try {
            const decoded = decodeReplay(codeToGame(code));
            return { decoded, steps: buildReplaySteps(decoded) };
        } catch (e) {
            console.error('Replay decode failed:', e);
            return null;
        }
    }, [code, mounted]);

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

    if (!mounted) {
        return null;
    }

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

    const btn = (label: string, onClick: () => void, title?: string) => (
        <button
            onClick={onClick}
            title={title}
            style={{
                minWidth: 40,
                padding: '0.35rem 0.5rem',
                fontSize: '0.95rem',
                cursor: 'pointer',
                borderRadius: 6,
                border: '1px solid rgba(255,255,255,0.25)',
                background: 'rgba(0,0,0,0.45)',
                color: 'var(--color-text-primary, #eee)',
            }}
        >
            {label}
        </button>
    );

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />

            <ReplayBoard decoded={decoded} step={step} />

            {/* replay controls — overlaid above the board, below center */}
            <div
                style={{
                    position: 'absolute',
                    bottom: 'max(8px, env(safe-area-inset-bottom))',
                    left: '50%',
                    transform: 'translateX(-50%)',
                    zIndex: 1100,
                    display: 'flex',
                    flexDirection: 'column',
                    alignItems: 'center',
                    gap: 6,
                    padding: '8px 12px',
                    borderRadius: 10,
                    background: 'rgba(0,0,0,0.5)',
                    backdropFilter: 'blur(2px)',
                    width: 'min(94vw, 440px)',
                }}
            >
                <div
                    className="text-shadow"
                    style={{
                        color: 'var(--color-text-primary)',
                        fontSize: '0.9rem',
                        minHeight: 32,
                        display: 'flex',
                        alignItems: 'center',
                        gap: 5,
                    }}
                >
                    <StepMessage step={step} />
                </div>

                <div style={{ display: 'flex', gap: 6, alignItems: 'center', width: '100%' }}>
                    {btn('⏮', () => { setPlaying(false); setStepIdx(0); })}
                    {btn('◀', () => { setPlaying(false); setStepIdx((i) => Math.max(i - 1, 0)); })}
                    {btn(playing ? '⏸' : '▶', () => setPlaying((p) => !p))}
                    {btn('▶▶', () => { setPlaying(false); setStepIdx((i) => Math.min(i + 1, lastIdx)); })}
                    {btn('⏭', () => { setPlaying(false); setStepIdx(lastIdx); })}
                    <input
                        type="range"
                        min={0}
                        max={lastIdx}
                        value={stepIdx}
                        onChange={(e) => { setPlaying(false); setStepIdx(Number(e.target.value)); }}
                        style={{ flex: 1, minWidth: 60 }}
                    />
                    <span
                        className="text-shadow"
                        style={{ color: 'var(--color-text-primary)', fontSize: '0.75rem', whiteSpace: 'nowrap' }}
                    >
                        {stepIdx + 1}/{steps.length}
                    </span>
                </div>
            </div>

            {/* home link, top-left like the in-game back button */}
            <Link
                href="/"
                className="text-shadow"
                style={{
                    position: 'absolute',
                    top: 'max(8px, env(safe-area-inset-top))',
                    left: 12,
                    zIndex: 1100,
                    color: 'var(--color-text-primary)',
                    fontSize: '0.85rem',
                    opacity: 0.85,
                }}
            >
                ← <Text id="foolish" />
            </Link>
        </div>
    );
};
