import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { useRouter } from 'next/navigation';
import type { ViewCard as Card } from '../state/view';
import { Text } from './Text';
import { SovietIcon } from './SovietIcon';
import { botDisplayName } from '../common/botName';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { ReplayServerProvider, useServer, useServerActions } from '../contexts/ServerContext';
import { AnimationProvider, useAnimation } from '../contexts/AnimationContext';
import { GameProvider } from '../contexts/GameContext';
import { DragProvider } from '../contexts/DragContext';
import { FernFractalProvider } from '../utils/fernFractal';
import { useLocalization } from '../contexts/LocalizationContext';
import { CardFace } from './GameDisplay/CardFace';
import { CardBack } from './GameDisplay/CardBack';
import { GameBoard } from './GameBoard';
import { Telestrator } from './Telestrator';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { animationFeed, AnimationSequenceMessage } from '../state/animationFeed';
import { bigintToBytes, bytesToBigint } from '@api/common/replay/codec.ts';
import { animHandLaidOut, ensureBotsAsync, kernelB32Decode, replaySummary } from '@sdk/ts/wasm/bots.ts';
import {
    buildReplayFrames,
    buildReverseFrames,
    preDealGame,
    stepTimes,
    ReplayFrame,
    ReplayGameState,
    REPLAY_STEP,
} from '../replay/frames';
import { splitReplayCode, decodeExtras, ReplayExtras } from '@api/common/replay/extras.ts';
import { OracleOverlay } from './OracleOverlay';
import { createOracleController, IOracleController } from '../oracle/oracleControllerFactory';
import { buildOracleJob, findDecisionIndex } from '../oracle/replayOracleInput';
import { OracleSnapshot } from '../oracle/types';
import { buildSpeeds, fmtDuration } from '../replay/speeds';
import { InlineCard } from './replay/InlineCards';
import { StepMessage, FoolMessage } from './replay/StepMessage';
import { RevealedHands } from './replay/RevealedHands';
import {
    IconBoutNext, IconBoutStart, IconEye, IconOracle, IconPause, IconPen,
    IconPlay, IconStepBack, IconStepForward,
} from './replay/ReplayIcons';

/** The key a replay's boards are held under (ReplayServerProvider, the frames' game id). */
const REPLAY_KEY = 'replay';

/**
 * Self-contained replay viewer: WWW.FOOLISH.CARDS/<base32> - the path segment
 * IS the entire game (decoded client-side, no auth, no database row).
 *
 * It IS the real game UI: the same display components, driven by the same
 * AnimationProvider, fed the same animation-sequence messages a live game
 * receives - just published into src/state/animationFeed from the decoded
 * integer instead of a supabase channel. Stepping forward plays the event
 * with its full animation; seeking commits the target state directly.
 */


interface StageProps {
    /** The fool's seat, or null for a code cut before the game ended. */
    fool: number | null;
    /** The replay code the frames were built from: the Oracle's position is read off it. */
    code: Uint8Array;
    frames: ReplayFrame[];
    reverses: (AnimationSequenceMessage | null)[];
    /** The link's id: it seeds the Oracle's decision ids. */
    gameId: string;
    names: string[] | null;
    times: (number | null)[];
}

const ReplayStage = ({ fool, code, frames, reverses, gameId, names, times }: StageProps) => {
    usePreventScroll();
    const { updateGameState } = useServerActions();
    const { isAnimating, resetAnimations } = useAnimation();
    const { t } = useLocalization();
    const router = useRouter();

    const [stepIdx, setStepIdx] = useState(-1); // -1 = pre-deal
    const [playing, setPlaying] = useState(false);
    const [reveal, setReveal] = useState(false);
    // Telestrator toggle: press to enter a red-pen overlay; press again to exit
    // AND clear it. Because the canvas is unmounted while `drawing` is false,
    // toggling off wipes the strokes and toggling on always starts blank.
    const [drawing, setDrawing] = useState(false);
    const toggleDrawing = useCallback(() => setDrawing((d) => !d), []);
    const speeds = useMemo(() => buildSpeeds(times), [times]);
    const [speedIdx, setSpeedIdx] = useState(0);
    // wall-clock target for the next autoplay move (realtime waits can exceed
    // setTimeout's 2^31 ms ceiling, so we tick against Date.now() instead)
    const [waitTarget, setWaitTarget] = useState<number | null>(null);
    const [now, setNow] = useState(() => Date.now());
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;
    const lastIdx = frames.length - 1;

    // ---- Infinite Oracle (docs/INFINITE_ORACLE_DESIGN.md) ------------------
    // A client-side octogen deliberation over the paused decision: strengths
    // stream in and sharpen. Analysis only arms once animation settles on a
    // decision step; the fleet stays warm across steps and is torn down on
    // unmount. StrictMode-safe (start/stop bump a run generation).
    const oracleRef = useRef<IOracleController | null>(null);
    const [oracleOpen, setOracleOpen] = useState(false);
    const [oracleMemory, setOracleMemory] = useState(true);
    const [oracleSnap, setOracleSnap] = useState<OracleSnapshot | null>(null);
    const oracleDecision = useMemo(() => findDecisionIndex(frames, stepIdx), [frames, stepIdx]);
    const getOracle = useCallback(() => {
        if (!oracleRef.current) oracleRef.current = createOracleController();
        return oracleRef.current;
    }, []);
    useEffect(() => {
        if (!oracleOpen) return;
        return getOracle().subscribe(setOracleSnap);
    }, [oracleOpen, getOracle]);
    useEffect(() => {
        if (!oracleOpen) return;
        const ctrl = getOracle();
        if (playing || isAnimating) { ctrl.stopCurrent(); return; }
        const job = buildOracleJob(frames, code, stepIdx, oracleMemory, gameId);
        if (!job) { setOracleSnap(null); return; }
        void ctrl.start(job);
        return () => ctrl.stopCurrent();
    }, [oracleOpen, stepIdx, isAnimating, playing, oracleMemory, code, frames, gameId, getOracle]);
    useEffect(() => () => { oracleRef.current?.dispose(); oracleRef.current = null; }, []);

    // publish one step's sequence into the feed; a fresh sequence_id (and a
    // deep copy) lets the same step replay after scrubbing back. Plain
    // counter + Math.random - crypto.randomUUID needs a secure context and
    // breaks LAN dev on iOS (http://192.168.x.x).
    const publishSeq = useRef(0);
    const publishStep = useCallback(
        (i: number) => {
            const seq: AnimationSequenceMessage = structuredClone(frames[i].seq);
            seq.sequence_id = `replay-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
            seq.timestamp = Date.now();
            (seq.events[0] as any)._nonce = seq.sequence_id; // defeat content dedup
            animationFeed.publish(seq);
            setStepIdx(i);
        },
        [frames],
    );

    const stepForward = useCallback(() => {
        setWaitTarget(null);
        if (stepRef.current >= lastIdx) {
            setPlaying(false);
            return;
        }
        publishStep(stepRef.current + 1);
    }, [lastIdx, publishStep]);

    // one step back plays the reverse sequence (cards fly home) and lands on
    // steps[i-1]; mirrors publishStep but with the inverted flight
    const publishReverse = useCallback(
        (i: number) => {
            const rev = reverses[i];
            if (!rev) return;
            const seq: AnimationSequenceMessage = structuredClone(rev);
            seq.sequence_id = `replay-rev-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
            seq.timestamp = Date.now();
            if (seq.events[0]) (seq.events[0] as any)._nonce = seq.sequence_id;
            animationFeed.publish(seq);
            setStepIdx(i - 1);
        },
        [reverses],
    );

    // seeking commits the target state directly: drop in-flight animations so
    // a stale event can't overwrite the jumped-to state afterwards
    const jumpTo = useCallback(
        (i: number) => {
            setPlaying(false);
            setWaitTarget(null);
            resetAnimations();
            const target = Math.max(0, Math.min(i, lastIdx));
            // The step's own board, straight from the kernel - no rebuild.
            updateGameState(REPLAY_KEY, frames[target].game);
            setStepIdx(target);
        },
        [frames, lastIdx, resetAnimations, updateGameState],
    );

    const stepBack = useCallback(() => {
        setPlaying(false);
        setWaitTarget(null);
        if (stepRef.current <= 0) {
            jumpTo(0);
            return;
        }
        publishReverse(stepRef.current);
    }, [publishReverse, jumpTo]);

    // Bout boundaries: a bout begins at the ATTACK that opens onto an empty
    // table (the previous step cleared it via pickup/discard, or it's the
    // game's first attack). Skip-to-bout jumps are animationless seeks.
    const boutStarts = useMemo(() => {
        const starts: number[] = [];
        for (let i = 0; i < frames.length; i++) {
            const opensEmpty = i === 0 || frames[i - 1].game.battles.length === 0;
            if (frames[i].kind === REPLAY_STEP.ATTACK && opensEmpty) starts.push(i);
        }
        return starts;
    }, [frames]);

    const nextBout = useCallback(() => {
        const from = Math.max(0, stepRef.current);
        const next = boutStarts.find((s) => s > from);
        jumpTo(next ?? lastIdx);
    }, [boutStarts, lastIdx, jumpTo]);

    // start of the current bout; if already sitting on it, fall back to the
    // previous bout's start (the usual transport-deck behaviour)
    const boutStart = useCallback(() => {
        const from = Math.max(0, stepRef.current);
        const here = [...boutStarts].reverse().find((s) => s <= from) ?? 0;
        if (here < from) {
            jumpTo(here);
            return;
        }
        const prev = [...boutStarts].reverse().find((s) => s < from);
        jumpTo(prev ?? 0);
    }, [boutStarts, jumpTo]);

    // opening deal on mount
    useEffect(() => {
        const timer = setTimeout(() => publishStep(0), 400);
        return () => clearTimeout(timer);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // autoplay scheduling: once the previous event's animation lands, pick the
    // delay before the next move - condensed beats by default, or the recorded
    // gap divided by the dial speed in realtime modes - and arm a wall-clock
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
            // 'c' (comment) cycles the telestrator: enter draw mode, then exit
            // + clear, then a fresh blank overlay again.
            if (e.key === 'c' || e.key === 'C') {
                toggleDrawing();
                return;
            }
            // While drawing, the transport keys are inert so a commentator's
            // keystrokes can't scrub or play the replay underneath the pen.
            if (drawing) return;
            if (e.key === 'ArrowRight') stepForward();
            if (e.key === 'ArrowLeft') stepBack();
            if (e.key === ' ') {
                e.preventDefault();
                setPlaying((p) => !p);
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [stepForward, stepBack, drawing, toggleDrawing]);

    const frame = frames[Math.max(0, stepIdx)];

    // VHS-deck transport button: a round, dark, bevelled knob (faint top
    // highlight + drop shadow); amber when active. Holds a glyph or a short
    // text label (the speed dial).
    const btn = (label: React.ReactNode, onClick: () => void, title?: string, active?: boolean) => (
        <button
            onClick={onClick}
            title={title}
            style={{
                width: 42,
                height: 42,
                flex: '0 0 auto',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                padding: 0,
                fontSize: '0.62rem',
                fontWeight: 700,
                letterSpacing: '0.03em',
                cursor: 'pointer',
                borderRadius: '50%',
                border: active ? '1px solid #E79743' : '1px solid rgba(255,255,255,0.16)',
                background: active
                    ? 'radial-gradient(circle at 50% 32%, rgba(231,151,67,0.42) 0%, rgba(231,151,67,0.18) 100%)'
                    : 'radial-gradient(circle at 50% 30%, #34343a 0%, #161618 100%)',
                color: active ? '#F0B36A' : 'rgba(232,232,232,0.92)',
                boxShadow:
                    'inset 0 1.5px 1px rgba(255,255,255,0.12), inset 0 -2px 3px rgba(0,0,0,0.5), 0 2px 4px rgba(0,0,0,0.55)',
            }}
        >
            {label}
        </button>
    );

    // The board lives in its own positioned region inset from the transport
    // controls (in the bottom-right corner) and the top status bar:
    // PlayerRing/DefenderShield/RevealedHands use percentage positions, which
    // resolve against the inset wrapper, so no seat is buried under a control.
    const boardInset: React.CSSProperties = {
        position: 'absolute',
        top: 'calc(44px + max(8px, env(safe-area-inset-top)))',
        left: 0,
        right: 0,
        bottom: 'calc(96px + max(8px, env(safe-area-inset-bottom)))',
    };

    return (
        <GameBoard
            boardInset={boardInset}
            overlay={reveal && <RevealedHands />}
            chrome={<>
            {/* Telestrator: a red-pen canvas that overlays the whole replay
                while drawing is on. Rendered in the board chrome (above the
                board + animation overlay); intercepts pointer events only
                while active. */}
            <Telestrator active={drawing} />

            {/* status bar, top-centre: move counter, timestamp, and what just
                happened - the readouts a VHS deck shows on its front display. */}
            <div
                style={{
                    position: 'absolute',
                    top: 'max(8px, env(safe-area-inset-top))',
                    left: '50%',
                    transform: 'translateX(-50%)',
                    zIndex: 1100,
                    display: 'flex',
                    flexDirection: 'column',
                    alignItems: 'center',
                    gap: 2,
                    maxWidth: 'min(92vw, 520px)',
                    pointerEvents: 'none',
                }}
            >
                <div
                    className="text-shadow"
                    style={{
                        display: 'flex',
                        alignItems: 'center',
                        gap: 8,
                        color: 'var(--color-text-primary)',
                        fontVariantNumeric: 'tabular-nums',
                        letterSpacing: '0.06em',
                    }}
                >
                    <span style={{ fontSize: '0.82rem', fontWeight: 700 }}>
                        {Math.max(0, stepIdx) + 1}
                        <span style={{ opacity: 0.55 }}> / {frames.length}</span>
                    </span>
                    {stepIdx >= 0 && times[stepIdx] !== null && (
                        <span style={{ fontSize: '0.7rem', opacity: 0.7, whiteSpace: 'nowrap' }}>
                            {new Date(times[stepIdx]! * 1000).toLocaleString(undefined, {
                                month: 'short',
                                day: 'numeric',
                                hour: '2-digit',
                                minute: '2-digit',
                                second: '2-digit',
                            })}
                        </span>
                    )}
                    {playing && waitTarget !== null && waitTarget - now > 4000 && (
                        <span style={{ fontSize: '0.72rem', opacity: 0.85, whiteSpace: 'nowrap' }}>
                            ⏳ {fmtDuration(waitTarget - now)}
                        </span>
                    )}
                </div>
                <div
                    className="text-shadow"
                    style={{
                        color: 'var(--color-text-primary)',
                        fontSize: '0.9rem',
                        minHeight: 26,
                        display: 'flex',
                        alignItems: 'center',
                        gap: 5,
                        textAlign: 'center',
                    }}
                >
                    {/* The last step is a real move, not a synthetic end marker,
                        so the closing line rides alongside it rather than
                        replacing it - the move that ended the game is worth
                        reading too. */}
                    {stepIdx >= 0 && <StepMessage frame={frame} names={names} />}
                    {stepIdx === lastIdx && <FoolMessage fool={fool} names={names} />}
                </div>
            </div>

            {/* transport controls, bottom-right corner - knobs float directly on
                the felt, no backing panel */}
            <div
                style={{
                    position: 'absolute',
                    bottom: 'max(10px, env(safe-area-inset-bottom))',
                    right: 'max(10px, env(safe-area-inset-right))',
                    zIndex: 1100,
                    display: 'flex',
                    flexDirection: 'column',
                    alignItems: 'flex-end',
                    gap: 10,
                    width: 'min(94vw, 360px)',
                }}
            >
                <input
                    type="range"
                    min={0}
                    max={lastIdx}
                    value={Math.max(0, stepIdx)}
                    onChange={(e) => jumpTo(Number(e.target.value))}
                    style={{ width: '100%' }}
                />
                <div style={{ display: 'flex', gap: 7, alignItems: 'center', justifyContent: 'flex-end', flexWrap: 'wrap' }}>
                    {btn(<IconBoutStart />, boutStart, t('replay_bout_start'))}
                    {btn(<IconStepBack />, stepBack, t('replay_step_back'))}
                    {btn(playing ? <IconPause /> : <IconPlay />, () => setPlaying((p) => !p), t(playing ? 'pause' : 'play'))}
                    {btn(<IconStepForward />, stepForward, t('replay_step_forward'))}
                    {btn(<IconBoutNext />, nextBout, t('replay_bout_next'))}
                    {btn(<IconEye />, () => setReveal((r) => !r), t(reveal ? 'hide_cards' : 'reveal_cards'), reveal)}
                    {btn(<IconPen />, toggleDrawing, t(drawing ? 'replay_draw_clear' : 'replay_draw'), drawing)}
                    <span data-testid="oracle-btn-wrap" style={{ opacity: oracleDecision != null ? 1 : 0.4 }}>
                        {btn(
                            <IconOracle />,
                            () => { if (oracleDecision != null) setOracleOpen((o) => !o); },
                            oracleDecision != null ? t('oracle_button_title') : t('oracle_no_decision'),
                            oracleOpen,
                        )}
                    </span>
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
                </div>
            </div>

            {/* Infinite Oracle panel - right-anchored, mounted in the board
                chrome so its mini-cards render inside the replay provider tree */}
            {oracleOpen && (
                <OracleOverlay
                    snapshot={oracleSnap}
                    onClose={() => setOracleOpen(false)}
                    onToggleMemory={() => setOracleMemory((m) => !m)}
                    onRetry={() => { const j = buildOracleJob(frames, code, stepIdx, oracleMemory, gameId); if (j) void getOracle().start(j); }}
                />
            )}

            {/* home button - the same little wood square as the in-game back
                button (btn-icon), positioned top-left by its own CSS */}
            <TexturedSurface
                as="button"
                seed={0.2}
                className="btn-icon btn-icon--left"
                onClick={() => router.push('/')}
                aria-label={t('back_to_home')}
            >
                <span className="btn-icon__symbol">{'<'}</span>
            </TexturedSurface>
            </>}
        />
    );
};

const buildReplayData = async (code: string) => {
    const { moves, extras: extrasCode } = splitReplayCode(code);
    const bytes = bigintToBytes(bytesToBigint(kernelB32Decode(moves)));
    await ensureBotsAsync();

    // The code at a glance, from the kernel: the seats, the fool, and how many
    // moves the extras time. A code that does not decode is not a replay.
    const summary = replaySummary(bytes);
    if (!summary) throw new Error('replay: the code does not decode');

    // extras (names + timing) are decoration: a malformed blob never
    // breaks the replay itself
    let extras: ReplayExtras = { names: null, startTime: null, moveGaps: null };
    if (extrasCode) {
        try {
            extras = decodeExtras(extrasCode, summary.numPlayers, summary.moves);
        } catch (e) {
            console.error('Replay extras ignored:', e);
        }
    }

    const names = extras.names;
    const fool = summary.fool >= 0 ? summary.fool : null;
    // The game, replayed by the engine: one frame per step, each the board the
    // engine really committed and the events it really produced.
    const frames = buildReplayFrames(bytes, REPLAY_KEY, names, { fool });
    const reverses = buildReverseFrames(frames);
    const initial = preDealGame(frames[0]);
    const times = stepTimes(frames, extras.startTime, extras.moveGaps);
    return { fool, code: bytes, frames, reverses, initial, names, times };
};

export const ReplayScreen = ({ code }: { code: string }) => {
    // Client-only: the game display reads window dimensions during render
    // (DefenderShield), so skip SSR/prerender entirely.
    const [mounted, setMounted] = useState(false);
    useEffect(() => setMounted(true), []);

    // The link names the replay (the Oracle seeds its decisions by it); the store
    // holds its boards under a short key of its own, as the tutorial's does. A
    // board's game id is a table's, and a share link - its moves and its extras -
    // is longer than a table's id may be: the kernel's board writer refuses it.
    const gameId = code.toLowerCase();

    // Async: the replay runs in bots.wasm, which the browser must compile
    // asynchronously. undefined = still decoding, null = failed.
    const [result, setResult] = useState<Awaited<ReturnType<typeof buildReplayData>> | null | undefined>(undefined);
    useEffect(() => {
        if (!mounted) return;
        let cancelled = false;
        setResult(undefined); // a code change must not keep showing the old replay
        buildReplayData(code)
            .then((r) => { if (!cancelled) setResult(r); })
            .catch((e) => {
                console.error('Replay decode failed:', e);
                if (!cancelled) setResult(null);
            });
        return () => { cancelled = true; };
    }, [code, mounted]);

    if (!mounted || result === undefined) {
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
            <ReplayServerProvider gameId={REPLAY_KEY} initialGame={result.initial}>
                <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                <ReplayStage
                                    fool={result.fool}
                                    code={result.code}
                                    frames={result.frames}
                                    reverses={result.reverses}
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
