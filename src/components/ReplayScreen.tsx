import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { Card, LOG_TYPE } from '../common/types';
import { Text } from './Text';
import { SovietIcon } from './SovietIcon';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { ReplayServerProvider, useServer } from '../contexts/ServerContext';
import { AnimationProvider, useAnimation } from '../contexts/AnimationContext';
import { GameProvider } from '../contexts/GameContext';
import { DragProvider } from '../contexts/DragContext';
import { FernFractalProvider } from '../utils/fernFractal';
import { useLocalization } from '../contexts/LocalizationContext';
import { CardFace } from './GameDisplay/CardFace';
import { CardBack } from './GameDisplay/CardBack';
import { TableBattles } from './GameDisplay/TableBattles';
import { PlayerRing } from './GameDisplay/PlayerRing';
import { DefenderShield } from './GameDisplay/DefenderShield';
import { DeckAndFlipped } from './GameDisplay/DeckAndFlipped';
import { DiscardPile } from './GameDisplay/DiscardPile';
import { AnimationOverlay } from './GameDisplay/AnimationOverlay';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { animationFeed, AnimationSequenceMessage } from '../state/animationFeed';
import { codeToGame } from '../replay/codec';
import { decodeReplay } from '../replay/decode';
import { DecodedReplay } from '../replay/core';
import {
    buildReplaySteps,
    stepToGame,
    ReplayStep,
    ReplayGameState,
} from '../replay/view';
import { buildReplaySequences, preDealGame } from '../replay/animate';
import { splitReplayCode, decodeExtras, ReplayExtras } from '../replay/extras';
import { INFO_TYPES } from '../replay/core';
import { stepTimes } from '../replay/view';

/**
 * Self-contained replay viewer: WWW.FOOLISH.CARDS/<base32> — the path segment
 * IS the entire game (decoded client-side, no auth, no database row).
 *
 * It IS the real game UI: the same display components, driven by the same
 * AnimationProvider, fed the same animation-sequence messages a live game
 * receives — just published into src/state/animationFeed from the decoded
 * integer instead of a supabase channel. Stepping forward plays the event
 * with its full animation; seeking commits the target state directly.
 */

/* Miniature cards rendered through the REAL CardFace/CardBack (native
 * 50×70 px, shrunk with a CSS transform) so they match the table exactly —
 * corner indices, center pip, theme styling, Soviet suit icons and all.
 * These render inside the replay's provider tree, which CardFace/CardBack
 * need (animation, styles, fern pattern). */
const CARD_W = 50;
const CARD_H = 70;

const InlineCard = ({ card, w = 22 }: { card: Card; w?: number }) => {
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
                <CardFace card={card} playerId="replay-inline" />
            </span>
        </span>
    );
};

const InlineCardBack = ({ w = 22 }: { w?: number }) => {
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
                <CardBack deckSize={1} />
            </span>
        </span>
    );
};

const seatName = (seat: number, names?: (string | null)[] | null) =>
    names?.[seat] || `P${seat + 1}`;

/* Playback speeds. '⚡' is the condensed default: recorded gaps clamped to
 * short beats. The ×N stops replay the RECORDED timing divided by N — at 1× a
 * three-day sulk between moves really takes three days (the countdown keeps
 * the screen honest), and for simulation games with nanosecond gaps the same
 * dial generates SLOW-MOTION stops (mult < 1) instead. Stops are derived from
 * the game's median gap: every power of ten that plays the median between
 * 0.2 s and 60 s, plus 1× always. */
interface SpeedStop {
    label: string;
    mult: number | null;
}

const fmtMult = (m: number): string => {
    if (m >= 1) return m >= 1e4 ? `1e${Math.round(Math.log10(m))}×` : `${m}×`;
    const exp = Math.round(Math.log10(m));
    return exp >= -2 ? `${m}×` : `1e${exp}×`;
};

const buildSpeeds = (times: (number | null)[]): SpeedStop[] => {
    const stops: SpeedStop[] = [{ label: '⚡', mult: null }];
    const gaps: number[] = [];
    for (let i = 1; i < times.length; i++) {
        const a = times[i - 1];
        const b = times[i];
        if (a !== null && b !== null && b > a) gaps.push(b - a);
    }
    if (gaps.length === 0) return stops; // no timing data: ⚡ only
    gaps.sort((x, y) => x - y);
    const median = gaps[Math.floor(gaps.length / 2)];

    const mults = new Set<number>([1]);
    for (let k = -12; k <= 12; k++) {
        const m = Math.pow(10, k);
        const beat = median / m;
        if (beat >= 0.2 && beat <= 60) mults.add(m);
    }
    [...mults]
        .sort((x, y) => x - y)
        .forEach((m) => stops.push({ label: fmtMult(m), mult: m }));
    return stops;
};

const fmtDuration = (ms: number): string => {
    const total = Math.max(0, Math.ceil(ms / 1000));
    const d = Math.floor(total / 86400);
    const h = Math.floor((total % 86400) / 3600);
    const m = Math.floor((total % 3600) / 60);
    const sec = total % 60;
    const hh = String(h).padStart(2, '0');
    const mm = String(m).padStart(2, '0');
    const ss = String(sec).padStart(2, '0');
    if (d > 0) return `${d}d ${hh}:${mm}:${ss}`;
    if (h > 0) return `${hh}:${mm}:${ss}`;
    return `${mm}:${ss}`;
};

const StepMessage = ({ step, names }: { step: ReplayStep; names: string[] | null }) => {
    const cards = (cs: Card[]) => (
        <span style={{ display: 'inline-flex', gap: 3, verticalAlign: 'middle' }}>
            {cs.map((c, i) => (
                <InlineCard key={i} card={c} />
            ))}
        </span>
    );
    const who = step.seat !== null ? <b>{seatName(step.seat, names)}</b> : null;

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
                    <b>{def >= 0 ? seatName(def, names) : '—'}</b>
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

/* A face-down card that squishes in a flex row like CardFace does — used for
 * hidden cards whose identity never surfaces in the replay. */
const SquishBack = () => (
    <span
        style={{
            flex: '0 0 32px',
            width: 32,
            height: 62,
            borderRadius: 5,
            background: '#B32929',
            border: '2px solid #7a1d1d',
            boxShadow: '0 1px 3px rgba(0,0,0,0.4)',
            display: 'block',
        }}
    />
);

/**
 * Reveal-hands overlay: every player's current hand face-up, positioned on
 * the same ellipse as PlayerRing (with the same viewer rotation: the replay
 * viewer is never a player, so self_index is -1 there and here) and rendered
 * exactly like the live self-hand — full CardFace components squished into a
 * flex row (ActionButtons' style), so squeezed cards drop to the same
 * thin-card layout as the original. Identities come from replay_hands —
 * retroactive knowledge of every card that ever surfaces; cards that never
 * get played stay face-down.
 */
const RevealedHands = () => {
    const game = useServer().game as ReplayGameState | null;
    if (!game || !game.replay_hands) return null;
    const n = game.players.length;

    return (
        <>
            {game.replay_hands.map((hand, index) => {
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
                            gap: 2,
                            // fixed card width, wrapping into (at most) two rows
                            width: Math.ceil(hand.length / 2) * 34 + 4,
                            zIndex: 60,
                            pointerEvents: 'none',
                        }}
                    >
                        {hand.map((c, i) =>
                            c ? (
                                <CardFace
                                    key={i}
                                    card={c}
                                    playerId="replay-reveal"
                                    style={{
                                        flex: '0 0 32px',
                                        width: 32,
                                        height: 62,
                                        position: 'relative',
                                    }}
                                />
                            ) : (
                                <SquishBack key={i} />
                            ),
                        )}
                    </div>
                );
            })}
        </>
    );
};

interface StageProps {
    decoded: DecodedReplay;
    steps: ReplayStep[];
    sequences: AnimationSequenceMessage[];
    gameId: string;
    names: string[] | null;
    times: (number | null)[];
}

const ReplayStage = ({ decoded, steps, sequences, gameId, names, times }: StageProps) => {
    usePreventScroll();
    const { updateGameState } = useServer();
    const { isAnimating, resetAnimations } = useAnimation();
    const { t } = useLocalization();

    const [stepIdx, setStepIdx] = useState(-1); // -1 = pre-deal
    const [playing, setPlaying] = useState(false);
    const [reveal, setReveal] = useState(false);
    const speeds = useMemo(() => buildSpeeds(times), [times]);
    const [speedIdx, setSpeedIdx] = useState(0);
    // wall-clock target for the next autoplay move (realtime waits can exceed
    // setTimeout's 2^31 ms ceiling, so we tick against Date.now() instead)
    const [waitTarget, setWaitTarget] = useState<number | null>(null);
    const [now, setNow] = useState(() => Date.now());
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;
    const lastIdx = steps.length - 1;

    // publish one step's sequence into the feed; a fresh sequence_id (and a
    // deep copy) lets the same step replay after scrubbing back
    const publishStep = useCallback(
        (i: number) => {
            const seq: AnimationSequenceMessage = JSON.parse(JSON.stringify(sequences[i]));
            seq.sequence_id = `replay-${i}-${crypto.randomUUID()}`;
            seq.timestamp = Date.now();
            (seq.events[0] as any)._nonce = seq.sequence_id; // defeat content dedup
            animationFeed.publish(seq);
            setStepIdx(i);
        },
        [sequences],
    );

    const stepForward = useCallback(() => {
        setWaitTarget(null);
        if (stepRef.current >= lastIdx) {
            setPlaying(false);
            return;
        }
        publishStep(stepRef.current + 1);
    }, [lastIdx, publishStep]);

    // seeking commits the target state directly: drop in-flight animations so
    // a stale event can't overwrite the jumped-to state afterwards
    const jumpTo = useCallback(
        (i: number) => {
            setPlaying(false);
            setWaitTarget(null);
            resetAnimations();
            const target = Math.max(0, Math.min(i, lastIdx));
            updateGameState(gameId, stepToGame(decoded, steps[target], gameId, names));
            setStepIdx(target);
        },
        [decoded, steps, gameId, names, lastIdx, resetAnimations, updateGameState],
    );

    // opening deal on mount
    useEffect(() => {
        const timer = setTimeout(() => publishStep(0), 400);
        return () => clearTimeout(timer);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // autoplay scheduling: once the previous event's animation lands, pick the
    // delay before the next move — condensed beats by default, or the recorded
    // gap divided by the dial speed in realtime modes — and arm a wall-clock
    // target. The ticker below fires it; this survives day-long waits.
    useEffect(() => {
        if (!playing || isAnimating || waitTarget !== null) return;
        if (stepIdx >= lastIdx) {
            setPlaying(false);
            return;
        }
        const mult = speeds[speedIdx % speeds.length].mult;
        let delay = 250;
        const a = stepIdx >= 0 ? times[stepIdx] : null;
        const b = times[stepIdx + 1];
        if (a !== null && b !== null && b !== undefined) {
            const gapMs = Math.max(0, (b - a) * 1000);
            delay =
                mult === null
                    ? Math.min(Math.max(gapMs, 150), 3000)
                    : Math.max(gapMs / mult, 30);
        }
        setWaitTarget(Date.now() + delay);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [playing, isAnimating, stepIdx, lastIdx, speedIdx, speeds, times, waitTarget]);

    // the ticker: fires the armed move and drives the countdown display
    useEffect(() => {
        if (waitTarget === null) return;
        const tick = () => {
            const t = Date.now();
            setNow(t);
            if (t >= waitTarget) {
                stepForward();
            }
        };
        tick();
        const interval = setInterval(tick, 250);
        return () => clearInterval(interval);
    }, [waitTarget, stepForward]);

    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'ArrowRight') stepForward();
            if (e.key === 'ArrowLeft') jumpTo(stepRef.current - 1);
            if (e.key === ' ') {
                e.preventDefault();
                setPlaying((p) => !p);
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [stepForward, jumpTo]);

    const step = steps[Math.max(0, stepIdx)];

    const btn = (label: React.ReactNode, onClick: () => void, title?: string, active?: boolean) => (
        <button
            onClick={onClick}
            title={title}
            style={{
                minWidth: 38,
                padding: '0.35rem 0.45rem',
                fontSize: '0.95rem',
                cursor: 'pointer',
                borderRadius: 6,
                border: active ? '1px solid #E79743' : '1px solid rgba(255,255,255,0.25)',
                background: active ? 'rgba(231,151,67,0.3)' : 'rgba(0,0,0,0.45)',
                color: 'var(--color-text-primary, #eee)',
            }}
        >
            {label}
        </button>
    );

    return (
        <>
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
                {reveal && <RevealedHands />}
            </div>

            <AnimationOverlay />

            {/* replay controls */}
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
                    width: 'min(94vw, 460px)',
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
                    {stepIdx >= 0 && <StepMessage step={step} names={names} />}
                    {playing && waitTarget !== null && waitTarget - now > 4000 && (
                        <span style={{ fontSize: '0.75rem', opacity: 0.85, whiteSpace: 'nowrap' }}>
                            ⏳ {fmtDuration(waitTarget - now)}
                        </span>
                    )}
                    {stepIdx >= 0 && times[stepIdx] !== null && (
                        <span
                            style={{
                                marginLeft: 'auto',
                                fontSize: '0.68rem',
                                opacity: 0.7,
                                whiteSpace: 'nowrap',
                            }}
                        >
                            {new Date(times[stepIdx]! * 1000).toLocaleString(undefined, {
                                month: 'short',
                                day: 'numeric',
                                hour: '2-digit',
                                minute: '2-digit',
                                second: '2-digit',
                            })}
                        </span>
                    )}
                </div>

                <div style={{ display: 'flex', gap: 6, alignItems: 'center', width: '100%' }}>
                    {btn('⏮', () => jumpTo(0))}
                    {btn('◀', () => jumpTo(stepRef.current - 1))}
                    {btn(playing ? '⏸' : '▶', () => setPlaying((p) => !p))}
                    {btn('▶▶', stepForward)}
                    {btn('⏭', () => jumpTo(lastIdx))}
                    {btn('👁', () => setReveal((r) => !r), t(reveal ? 'hide_cards' : 'reveal_cards'), reveal)}
                    {speeds.length > 1 &&
                        btn(
                            speeds[speedIdx % speeds.length].label,
                            () => {
                                setWaitTarget(null); // re-arm with the new speed
                                setSpeedIdx((i) => (i + 1) % speeds.length);
                            },
                            t('playback_speed'),
                            speeds[speedIdx % speeds.length].mult !== null,
                        )}
                    <input
                        type="range"
                        min={0}
                        max={lastIdx}
                        value={Math.max(0, stepIdx)}
                        onChange={(e) => jumpTo(Number(e.target.value))}
                        style={{ flex: 1, minWidth: 50 }}
                    />
                    <span
                        className="text-shadow"
                        style={{ color: 'var(--color-text-primary)', fontSize: '0.75rem', whiteSpace: 'nowrap' }}
                    >
                        {Math.max(0, stepIdx) + 1}/{steps.length}
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
        </>
    );
};

export const ReplayScreen = ({ code }: { code: string }) => {
    // Client-only: the game display reads window dimensions during render
    // (DefenderShield), so skip SSR/prerender entirely.
    const [mounted, setMounted] = useState(false);
    useEffect(() => setMounted(true), []);

    const gameId = code.toLowerCase();

    const result = useMemo(() => {
        if (!mounted) return null;
        try {
            const { moves, extras: extrasCode } = splitReplayCode(code);
            const decoded = decodeReplay(codeToGame(moves));
            const steps = buildReplaySteps(decoded);

            // extras (names + timing) are decoration: a malformed blob never
            // breaks the replay itself
            let extras: ReplayExtras = { names: null, startTime: null, moveGaps: null };
            if (extrasCode) {
                try {
                    const moveCount = decoded.logs.filter((l) =>
                        INFO_TYPES.includes(l.log_type),
                    ).length;
                    extras = decodeExtras(extrasCode, decoded.playerCount, moveCount);
                } catch (e) {
                    console.error('Replay extras ignored:', e);
                }
            }

            const names = extras.names;
            const sequences = buildReplaySequences(decoded, steps, gameId, names);
            const initial = preDealGame(decoded, steps[0], gameId, names);
            const times = stepTimes(steps, extras.startTime, extras.moveGaps);
            return { decoded, steps, sequences, initial, names, times };
        } catch (e) {
            console.error('Replay decode failed:', e);
            return null;
        }
    }, [code, gameId, mounted]);

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

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            <ReplayServerProvider gameId={gameId} initialGame={result.initial}>
                <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                <ReplayStage
                                    decoded={result.decoded}
                                    steps={result.steps}
                                    sequences={result.sequences}
                                    gameId={gameId}
                                    names={result.names}
                                    times={result.times}
                                />
                            </DragProvider>
                        </GameProvider>
                    </AnimationProvider>
                </FernFractalProvider>
            </ReplayServerProvider>
        </div>
    );
};
