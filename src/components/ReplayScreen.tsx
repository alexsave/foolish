import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { useRouter } from 'next/navigation';
import { Card } from '@shared/types.ts';
import { Text } from './Text';
import { SovietIcon } from './SovietIcon';
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
import { bigintToBytes, codeToGame } from '@shared/replay/codec.ts';
import { decodeReplay } from '@shared/replay/decode.ts';
import { DecodedReplay } from '@shared/replay/core.ts';
import { ensureBotsAsync } from '@shared/wasm/bots.ts';
import {
    buildReplayFrames,
    buildReverseFrames,
    preDealGame,
    stepTimes,
    ReplayFrame,
    ReplayGameState,
    REPLAY_STEP,
} from '../replay/frames';
import { splitReplayCode, decodeExtras, ReplayExtras } from '@shared/replay/extras.ts';
import { INFO_TYPES } from '@shared/replay/core.ts';
import { OracleOverlay } from './OracleOverlay';
import { OracleController } from '../oracle/OracleController';
import { buildOracleJob, findDecisionIndex } from '../oracle/replayOracleInput';
import { OracleSnapshot } from '../oracle/types';

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
        <CardFace card={card} playerId="replay-inline" />
    </ScaledCard>
);

const InlineCardBack = ({ w = 22 }: { w?: number }) => (
    <ScaledCard w={w}>
        <CardBack deckSize={1} />
    </ScaledCard>
);

const seatName = (seat: number, names?: (string | null)[] | null) =>
    names?.[seat] || `P${seat + 1}`;

/* Playback speeds. 'AUTO' is the condensed default: recorded gaps clamped to
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
    const stops: SpeedStop[] = [{ label: 'AUTO', mult: null }];
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

/* What just happened, in the viewer's language. The kind comes from the kernel
 * (frames.ts) rather than from the frame's events, because on the wire an attack
 * and a pass are one event type told apart only by a reconstructed English
 * sentence — and this line is localized, so that sentence is no use here anyway.
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
        case REPLAY_STEP.ATTACK:
            return (
                <span>
                    {who} <SovietIcon name="sword" size={13} /> <Text id="attack" />: {cards(frame.cards)}
                </span>
            );
        case REPLAY_STEP.COVER:
            return (
                <span>
                    {who} <Text id="cover" />: {cards(frame.cards)} → {frame.target && cards([frame.target])}
                </span>
            );
        case REPLAY_STEP.PASS:
            return (
                <span>
                    {who} <Text id="pass" />: {cards(frame.cards)}
                </span>
            );
        case REPLAY_STEP.PICKUP:
            return (
                <span>
                    {who} <Text id="pickup" /> ({frame.count})
                </span>
            );
        case REPLAY_STEP.GOOD:
            return (
                <span>
                    {who} ✓ <Text id="good" />
                </span>
            );
        // The bout closed: everyone still in said good, and the table went to
        // the discard. One step, because it is one thing that happened.
        case REPLAY_STEP.ROUND_END:
            return (
                <span>
                    ✓ <Text id="good" /> — {frame.count} <Text id="discarded" />
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

/* Stable per-card key for the prefer-local-order reconciliation. Known cards
 * are identified by suit+value; face-down (null) slots are indistinguishable,
 * so they collapse onto a single bucket and are matched positionally by the
 * merge below. */
const handCardKey = (c: Card | null) => (c ? `${c.suit}-${c.value}` : 'hidden');

/* Mirror of ServerContext.mergeHandOrder, generalised to the replay's
 * (Card | null)[] hands: keep the viewer's preferred ordering for cards that
 * still exist, append cards that appeared since, and drop ones that left —
 * so a local rearrangement survives scrubbing/stepping the way the live
 * game's local hand order survives server updates. Face-down slots are
 * reconciled by count (they carry no identity). */
const mergeReplayHandOrder = (
    preferred: (Card | null)[],
    current: (Card | null)[],
): (Card | null)[] => {
    if (preferred.length === 0) return current;

    // Remaining counts of each card identity in the current hand.
    const remaining = new Map<string, number>();
    for (const c of current) {
        const k = handCardKey(c);
        remaining.set(k, (remaining.get(k) ?? 0) + 1);
    }

    const result: (Card | null)[] = [];
    // Preserved cards keep their preferred positions...
    for (const c of preferred) {
        const k = handCardKey(c);
        const left = remaining.get(k) ?? 0;
        if (left > 0) {
            result.push(c);
            remaining.set(k, left - 1);
        }
    }
    // ...then anything new (by surviving count) appends at the end.
    for (const c of current) {
        const k = handCardKey(c);
        const left = remaining.get(k) ?? 0;
        if (left > 0) {
            result.push(c);
            remaining.set(k, left - 1);
        }
    }
    return result;
};

/**
 * Reveal-hands overlay: every player's current hand face-up, positioned on
 * the same ellipse as PlayerRing (with the same viewer rotation: the replay
 * viewer is never a player, so self_index is -1 there and here). Cards are
 * the real CardFace/CardBack rendered at native 50×70 and scaled to 80%, so
 * they keep full card proportions — corner indices and center pip — instead
 * of the squished thin layout. Two centered rows per hand. Identities come
 * from replay_hands — retroactive knowledge of every card that ever
 * surfaces; cards that never get played stay face-down.
 *
 * Every hand here is drag-to-rearrangeable: the viewer can reorder the cards
 * within ANY player's hand exactly like reordering their own hand in the live
 * game. This is purely cosmetic and entirely client-side — there is no server
 * in a replay, so nothing is committed anywhere; we only keep a per-seat
 * "prefer local order" overlay (localOrders) that the render prefers, falling
 * back to the underlying replay_hands order and reconciling against the
 * current hand as the replay is scrubbed (see mergeReplayHandOrder). Cards are
 * NOT selectable or playable on the replay screen — only reordering.
 */
const RevealedHands = () => {
    const game = useServer().game as ReplayGameState | null;

    // Prefer-local-order overlay, keyed by seat index. Each entry is the
    // viewer's preferred ordering of that seat's hand; the render reconciles it
    // against the seat's current replay_hands order on every step/seek.
    const [localOrders, setLocalOrders] = useState<{ [seat: number]: (Card | null)[] }>({});

    // Active drag (STATE, not a ref, so the held card can render faded in place
    // exactly like the live hand): which seat's hand and which displayed slot is
    // being dragged. Reorder-only — no selection, no play, no cross-seat moves.
    const [drag, setDrag] = useState<{ seat: number; index: number } | null>(null);

    const displayHands = useMemo(() => {
        if (!game || !game.replay_hands) return null;
        return game.replay_hands.map((hand, index) =>
            mergeReplayHandOrder(localOrders[index] ?? [], hand),
        );
    }, [game, localOrders]);

    // While a card is held, hovering another slot in the SAME seat swaps the two
    // — the live hand's real-time swap-on-hover (elementsFromPoint + data-*
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
    const n = game.players.length;

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
                                    // swap instead of repainting content in place —
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

/* Flat VHS-deck transport glyphs — geometric, single-colour (currentColor),
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
/* Telestrator pen — a simple diagonal marker; the active state tints the
   whole knob amber like the other transport toggles. */
const IconPen = () => (
    <Glyph>
        <path d="M16.5 3.5a2 2 0 0 1 2.8 2.8L8.7 16.9 4 18.5l1.6-4.7L16.5 3.5Z" />
    </Glyph>
);
/* Oracle — a crystal ball on its stand; active state tints the knob amber. */
const IconOracle = () => (
    <Glyph>
        <circle cx={12} cy={10} r={6} />
        <path d="M6.5 17.5h11L19 21H5l1.5-3.5Z" />
        <circle cx={9.7} cy={8} r={1.6} fill="#161618" />
    </Glyph>
);

interface StageProps {
    decoded: DecodedReplay;
    frames: ReplayFrame[];
    reverses: (AnimationSequenceMessage | null)[];
    gameId: string;
    names: string[] | null;
    times: (number | null)[];
}

const ReplayStage = ({ decoded, frames, reverses, gameId, names, times }: StageProps) => {
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
    const oracleRef = useRef<OracleController | null>(null);
    const [oracleOpen, setOracleOpen] = useState(false);
    const [oracleMemory, setOracleMemory] = useState(true);
    const [oracleSnap, setOracleSnap] = useState<OracleSnapshot | null>(null);
    const oracleDecision = useMemo(() => findDecisionIndex(frames, stepIdx), [frames, stepIdx]);
    const getOracle = useCallback(() => {
        if (!oracleRef.current) oracleRef.current = new OracleController();
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
        const job = buildOracleJob(frames, decoded, stepIdx, oracleMemory, gameId);
        if (!job) { setOracleSnap(null); return; }
        void ctrl.start(job);
        return () => ctrl.stopCurrent();
    }, [oracleOpen, stepIdx, isAnimating, playing, oracleMemory, decoded, frames, gameId, getOracle]);
    useEffect(() => () => { oracleRef.current?.dispose(); oracleRef.current = null; }, []);
    const renderOracleCard = useCallback(
        (card: Card, w = 18) => <InlineCard card={card} w={w} />, []);

    // publish one step's sequence into the feed; a fresh sequence_id (and a
    // deep copy) lets the same step replay after scrubbing back. Plain
    // counter + Math.random — crypto.randomUUID needs a secure context and
    // breaks LAN dev on iOS (http://192.168.x.x).
    const publishSeq = useRef(0);
    const publishStep = useCallback(
        (i: number) => {
            const seq: AnimationSequenceMessage = JSON.parse(JSON.stringify(frames[i].seq));
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
            const seq: AnimationSequenceMessage = JSON.parse(JSON.stringify(rev));
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
            // The step's own board, straight from the kernel — no rebuild.
            updateGameState(gameId, frames[target].game);
            setStepIdx(target);
        },
        [frames, gameId, lastIdx, resetAnimations, updateGameState],
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
            const opensEmpty = i === 0 || frames[i - 1].game.table_battles.length === 0;
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
                happened — the readouts a VHS deck shows on its front display. */}
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
                        replacing it — the move that ended the game is worth
                        reading too. */}
                    {stepIdx >= 0 && <StepMessage frame={frame} names={names} />}
                    {stepIdx === lastIdx && <FoolMessage fool={decoded.fool} names={names} />}
                </div>
            </div>

            {/* transport controls, bottom-right corner — knobs float directly on
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

            {/* Infinite Oracle panel — right-anchored, mounted in the board
                chrome so its mini-cards render inside the replay provider tree */}
            {oracleOpen && (
                <OracleOverlay
                    snapshot={oracleSnap}
                    onClose={() => setOracleOpen(false)}
                    onToggleMemory={() => setOracleMemory((m) => !m)}
                    onRetry={() => { const j = buildOracleJob(frames, decoded, stepIdx, oracleMemory, gameId); if (j) void getOracle().start(j); }}
                    renderCard={renderOracleCard}
                />
            )}

            {/* home button — the same little wood square as the in-game back
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

const buildReplayData = async (code: string, gameId: string) => {
    const { moves, extras: extrasCode } = splitReplayCode(code);
    const x = codeToGame(moves);
    await ensureBotsAsync();

    // The kernel's own decode, for the two things the frames don't carry: who
    // the fool was, and the public log stream the Oracle reasons from. Not a
    // projection — it is one wasm call into the same replay.c the frames come
    // from.
    const decoded = await decodeReplay(x);

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
    // The game, replayed by the engine: one frame per step, each the board the
    // engine really committed and the events it really produced.
    const frames = buildReplayFrames(bigintToBytes(x), gameId, names, decoded.fool);
    const reverses = buildReverseFrames(frames);
    const initial = preDealGame(frames[0]);
    const times = stepTimes(frames, extras.startTime, extras.moveGaps);
    return { decoded, frames, reverses, initial, names, times };
};

export const ReplayScreen = ({ code }: { code: string }) => {
    // Client-only: the game display reads window dimensions during render
    // (DefenderShield), so skip SSR/prerender entirely.
    const [mounted, setMounted] = useState(false);
    useEffect(() => setMounted(true), []);

    const gameId = code.toLowerCase();

    // Async: decodeReplay runs in the rules kernel, which the browser must
    // compile asynchronously. undefined = still decoding, null = failed.
    const [result, setResult] = useState<Awaited<ReturnType<typeof buildReplayData>> | null | undefined>(undefined);
    useEffect(() => {
        if (!mounted) return;
        let cancelled = false;
        setResult(undefined); // a code change must not keep showing the old replay
        buildReplayData(code, gameId)
            .then((r) => { if (!cancelled) setResult(r); })
            .catch((e) => {
                console.error('Replay decode failed:', e);
                if (!cancelled) setResult(null);
            });
        return () => { cancelled = true; };
    }, [code, gameId, mounted]);

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
            <ReplayServerProvider gameId={gameId} initialGame={result.initial}>
                <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                <ReplayStage
                                    decoded={result.decoded}
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
