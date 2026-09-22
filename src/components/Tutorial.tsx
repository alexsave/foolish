'use client';

import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { useRouter } from 'next/navigation';
import { PLAYER_STATUS, type TableView, type ViewCard as Card } from '../state/view';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { AuthContext } from '../contexts/AuthContext';
import { ReplayServerProvider, useServer } from '../contexts/ServerContext';
import { AnimationProvider, AnimationContext, useAnimation } from '../contexts/AnimationContext';
import { GameProvider, useGame } from '../contexts/GameContext';
import { DragProvider } from '../contexts/DragContext';
import { FernFractalProvider } from '../utils/fernFractal';
import { useLocalization } from '../contexts/LocalizationContext';
import { TutorialHintProvider, TutorialHint } from '../contexts/TutorialHintContext';
import { GameBoard } from './GameBoard';
import { usePreventScroll } from '../hooks/usePreventScroll';
import { animationFeed, AnimationSequenceMessage } from '../state/animationFeed';
import { bigintToBytes, bytesToBigint } from '@api/common/replay/codec.ts';
import { ensureBotsAsync, kernelB32Decode, replaySummary, type ReplaySummary } from '@sdk/ts/wasm/bots.ts';
import {
    buildReplayFrames, preDealGame, ReplayFrame, ReplayGameState, REPLAY_STEP,
} from '../replay/frames';
import { canCoverCards } from '../utils/gameValidation';
import type { StringId } from '../localization/strings';
import { TUTORIAL_MOVES_CODE, TUTORIAL_NAMES } from './tutorialGame';

const LEARNER_SEAT = 0;
const GAME_ID = 'tutorial';
const RESULT = { game_id: GAME_ID };

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;
const LEARNER_KINDS: number[] = [
    REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP, REPLAY_STEP.GOOD,
];

/* A good that CLOSES the bout is not attributed to anyone: v6 records the round
 * ending, not who ended it (replay.c apply_round_end emits seat -1), because the
 * transition belongs to every attacker who had not yet spoken. So when the
 * learner's own good is the one that closes a bout, the step arrives seat-less
 * and the tutorial has to recognise it from the board instead: seat 0 was in,
 * was not defending, and had not said good — so the good the round is waiting on
 * is theirs to give. */
const learnerOwesGood = (prev: ReplayFrame | undefined): boolean => {
    if (!prev) return false;
    const me = prev.game.seats[LEARNER_SEAT];
    return !!me
        && me.status !== PLAYER_STATUS.OUT
        && prev.game.defender !== LEARNER_SEAT
        && ((prev.game.goodMask >>> LEARNER_SEAT) & 1) === 0;
};

/* The learner's hand, in a stable display order. The frames are built for seat 0
 * (buildReplayFrames viewer), so the board's own hand is already the kernel's
 * masked view - the same one a real player in that seat is served, with no
 * account behind the seat (the board's mySeat says whose hand it is). All this
 * adds is the sort:
 * trumps last, then by value, so the hand does not reshuffle itself under the
 * learner as they play. */
function sortedHand(state: TableView, powerSuit: number): Card[] {
    const hand = [...state.myHand];
    hand.sort((a, b) => {
        const ta = a.suit === powerSuit ? 1 : 0, tb = b.suit === powerSuit ? 1 : 0;
        return ta - tb || a.value - b.value || a.suit - b.suit;
    });
    return hand;
}
const mkWithSelf = (powerSuit: number) => <T extends TableView>(state: T): T =>
    (!state || !state.seats ? state : ({ ...state, myHand: sortedHand(state, powerSuit) } as T));

/* ----------------------------- concept beats ------------------------------- */

/* The tutorial's own narration, in the one string table (c/i18n/keys.h, the
 * `tut_` keys). It used to be a second table in this directory with three
 * languages of its own; a learner reading one of the other twenty-two was
 * taught in English while the board around them spoke their language. The
 * narration is also why these keys are NOT the board's: `cover` is a button a
 * player presses, `tut_cover` is a sentence explaining what covering is, and
 * the two are different words in most languages. */
type TutKey = Extract<StringId, `tut_${string}`>;

interface Beat { at: number; key: TutKey; extra?: TutKey; name?: string; }

/* A concept is taught the first time the game shows it.
 *
 * A step is one ACTION now, and an action brings its consequences with it, so
 * the concepts that used to be steps of their own are read off the step's own
 * EVENTS instead: a refill is what a draw looks like, an out is what going out
 * looks like. Both are the kernel's own events — the beat asks the frame what
 * happened, it does not re-derive it.
 *
 * At most one beat shows at a time (the latest at or before the cursor), so a
 * step that teaches two things at once — a round end is both "good" and
 * "discard" — would silently drop one. Rather than lose it, that step gets ONE
 * beat carrying both. */
function buildBeats(frames: ReplayFrame[], summary: ReplaySummary, names: string[]): Beat[] {
    const beats: Beat[] = [];
    const seen = new Set<string>();
    const once = (k: string) => (seen.has(k) ? false : (seen.add(k), true));
    const ps = summary.powerSuit;
    const fa = summary.firstAttacker;
    beats.push({ at: 0, key: fa === LEARNER_SEAT ? 'tut_first_attacker_you' : 'tut_first_attacker', name: names[fa] });

    const has = (f: ReplayFrame, type: string) => f.seq.events.some((e) => e.type === type);

    for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        const prev = frames[i - 1];
        switch (f.kind) {
            case REPLAY_STEP.ATTACK:
                if (prev && prev.game.battles.length > 0 && once('throwIn'))
                    beats.push({ at: i, key: 'tut_throw_in', extra: 'tut_capacity' });
                break;
            case REPLAY_STEP.COVER: {
                if (once('cover')) beats.push({ at: i, key: 'tut_cover', extra: 'tut_stack_rule' });
                // EVERY pair of the move: one step can take several attacks
                // (buildReplayFrames merges a multi-cover the wire split), and
                // the trump may be spent on any of them - reading cards[0]
                // against a single target would miss it and, worse, compare one
                // pair's card with another pair's attack.
                if ((f.pairs ?? []).some((pr) => pr.card.suit === ps && pr.target.suit !== ps)
                    && once('trumpCover'))
                    beats.push({ at: i, key: 'tut_trump_cover' });
                break;
            }
            case REPLAY_STEP.PASS: if (once('pass')) beats.push({ at: i, key: 'tut_pass' }); break;
            case REPLAY_STEP.PICKUP: if (once('pickup')) beats.push({ at: i, key: 'tut_pickup' }); break;
            case REPLAY_STEP.GOOD: if (once('good')) beats.push({ at: i, key: 'tut_good' }); break;
            case REPLAY_STEP.ROUND_END: {
                // The bout closed: everyone said good, and the table was binned.
                const g = once('good'), d = once('discard');
                if (g) beats.push({ at: i, key: 'tut_good', extra: d ? 'tut_discard' : undefined });
                else if (d) beats.push({ at: i, key: 'tut_discard' });
                break;
            }
        }
        // Draws and outs ride the action that caused them.
        if (has(f, 'refill') && once('draw')) beats.push({ at: i, key: 'tut_draw' });
        if (has(f, 'out') && once('out')) {
            const outEv = f.seq.events.find((e) => e.type === 'out');
            const seat = outEv?.seat ?? -1;
            beats.push({ at: i, key: 'tut_out', name: names[seat >= 0 ? seat : 0] });
        }
        if (f.game.deckCount === 0 && !f.game.hasFlipped && once('deckEmpty'))
            beats.push({ at: i, key: 'tut_deck_empty' });
        if (i === frames.length - 1) beats.push({ at: i, key: 'tut_fool', name: names[summary.fool] });
    }
    beats.sort((a, b) => a.at - b.at);
    return beats;
}

/* The learner's pending move, derived from the next scripted step. */
interface Move {
    kind: number;
    cards: Card[];          // cards to highlight in hand
    target: Card | null;    // attack card to cover (COVER)
    action: string | null;  // wooden button to glow, or null when a drag is needed
    mode: 'button' | 'drag';
}

/* ------------------------- playback (state + override) --------------------- */
interface TutPlay {
    names: string[];
    summary: ReplaySummary;
    stepIdx: number;
    awaiting: boolean;
    finished: boolean;
    beatText: string;
    move: Move | null;
    skipToEnd: () => void;
    onExit: () => void;
}
const TutPlayContext = createContext<TutPlay | null>(null);
const useTutPlay = () => useContext(TutPlayContext)!;

interface PlaybackProps {
    summary: ReplaySummary;
    frames: ReplayFrame[];
    names: string[];
    onExit: () => void;
}

const TutorialPlayback = ({ summary, frames, names, onExit }: PlaybackProps) => {
    const { updateGameState, view: game } = useServer();
    const real = useAnimation();
    const { isAnimating } = real;
    const { t } = useLocalization();
    const withSelf = useMemo(() => mkWithSelf(summary.powerSuit), [summary.powerSuit]);

    const [stepIdx, setStepIdx] = useState(-1);
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;
    const lastIdx = frames.length - 1;
    const publishSeq = useRef(0);

    const beats = useMemo(() => buildBeats(frames, summary, names), [frames, summary, names]);
    // The learner's own move: a step they acted on, or the seat-less round end
    // their good is what the table is waiting for (see learnerOwesGood).
    const isLearnerStep = useCallback(
        (i: number) => {
            if (i < 0 || i > lastIdx) return false;
            const f = frames[i];
            if (f.kind === REPLAY_STEP.ROUND_END) return learnerOwesGood(frames[i - 1]);
            return f.seat === LEARNER_SEAT && LEARNER_KINDS.includes(f.kind);
        },
        [frames, lastIdx],
    );

    // A published step is on its way until its animation starts: the feed queues
    // it in this render and the queue picks it up in the next, so for one commit
    // the step is set and nothing is animating yet. The hint must not read that
    // commit as the board it points at, or it flashes before the deal lands.
    const [landing, setLanding] = useState(false);
    useEffect(() => { if (isAnimating) setLanding(false); }, [isAnimating]);

    const publishStep = useCallback((i: number) => {
        const seq: AnimationSequenceMessage = structuredClone(frames[i].seq);
        seq.sequence_id = `tut-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
        seq.timestamp = Date.now();
        if (seq.events[0]) (seq.events[0] as any)._nonce = seq.sequence_id;
        animationFeed.publish(seq);
        setLanding(seq.events.length > 0);
        setStepIdx(i);
    }, [frames]);

    // performing a (correct) action advances the scripted game
    const tryAdvance = useCallback((kind: number): boolean => {
        const next = stepRef.current + 1;
        if (next > lastIdx) return false;
        const f = frames[next];
        // A good that closes the bout arrives as a seat-less ROUND_END; it is
        // still the learner's good to press.
        const matches = kind === REPLAY_STEP.GOOD
            ? (f.kind === REPLAY_STEP.GOOD && f.seat === LEARNER_SEAT)
                || (f.kind === REPLAY_STEP.ROUND_END && learnerOwesGood(frames[next - 1]))
            : f.seat === LEARNER_SEAT && f.kind === kind;
        if (!matches) return false;
        publishStep(next);
        return true;
    }, [frames, lastIdx, publishStep]);

    // opening deal
    useEffect(() => {
        const t = setTimeout(() => publishStep(0), 450);
        return () => clearTimeout(t);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // auto-advance everything except the learner's own move
    useEffect(() => {
        if (stepIdx < 0 || stepIdx >= lastIdx || isAnimating) return;
        if (isLearnerStep(stepIdx + 1)) return;
        const isBeat = beats.some((b) => b.at === stepIdx);
        const delay = stepIdx === 0 ? 1900 : isBeat ? 2300 : 850;
        const t = setTimeout(() => publishStep(stepIdx + 1), delay);
        return () => clearTimeout(t);
    }, [stepIdx, lastIdx, isAnimating, isLearnerStep, beats, publishStep]);

    const skipToEnd = useCallback(() => {
        real.resetAnimations();
        updateGameState(GAME_ID, withSelf(frames[lastIdx].game));
        setLanding(false);
        setStepIdx(lastIdx);
    }, [frames, lastIdx, real, updateGameState, withSelf]);

    const awaiting = stepIdx >= 0 && stepIdx < lastIdx && !isAnimating && !landing && isLearnerStep(stepIdx + 1);
    const pending = awaiting ? frames[stepIdx + 1] : null;
    const finished = stepIdx >= lastIdx;

    // derive the learner's move (which cards/button, or a drag) from the step
    const move: Move | null = useMemo(() => {
        if (!pending) return null;
        // A round end reaching here IS the learner's good (isLearnerStep gates it).
        const k = pending.kind === REPLAY_STEP.ROUND_END ? REPLAY_STEP.GOOD : pending.kind;
        if (k === REPLAY_STEP.ATTACK || k === REPLAY_STEP.PASS)
            return { kind: k, cards: pending.cards, target: null, action: k === REPLAY_STEP.ATTACK ? 'attack' : 'pass', mode: 'button' };
        if (k === REPLAY_STEP.PICKUP)
            return { kind: k, cards: [], target: null, action: 'pickup', mode: 'button' };
        if (k === REPLAY_STEP.GOOD)
            return { kind: k, cards: [], target: null, action: 'good', mode: 'button' };
        if (k === REPLAY_STEP.COVER) {
            const coverCard = pending.cards[0];
            // the Cover button only appears for an unambiguous single-target cover;
            // otherwise the learner drags the card onto the specific attack.
            const canBtn = !!game && canCoverCards(game as any, [coverCard]);
            return { kind: k, cards: [coverCard], target: pending.target, action: canBtn ? 'cover' : null, mode: canBtn ? 'button' : 'drag' };
        }
        return null;
    }, [pending, game]);

    const activeBeat = useMemo(() => {
        let b: Beat | null = null;
        for (const beat of beats) if (beat.at <= Math.max(0, stepIdx)) b = beat;
        return b;
    }, [beats, stepIdx]);
    const beatText = activeBeat
        ? t(activeBeat.key, activeBeat.name ? { name: activeBeat.name } : undefined) +
          (activeBeat.extra ? ' ' + t(activeBeat.extra) : '')
        : '';

    const hint: TutorialHint | null = move
        ? { cards: move.cards, action: move.action, targetCard: move.target }
        : null;

    const animValue = useMemo(() => ({
        ...real,
        attack: async () => { tryAdvance(REPLAY_STEP.ATTACK); return RESULT; },
        pass: async () => { tryAdvance(REPLAY_STEP.PASS); return RESULT; },
        pickup: async () => { tryAdvance(REPLAY_STEP.PICKUP); return RESULT; },
        cover: async () => { tryAdvance(REPLAY_STEP.COVER); return RESULT; },
        good: async () => { tryAdvance(REPLAY_STEP.GOOD); return RESULT; },
    }), [real, tryAdvance]);

    const play: TutPlay = { names, summary, stepIdx, awaiting, finished, beatText, move, skipToEnd, onExit };

    return (
        <TutPlayContext.Provider value={play}>
            <AnimationContext.Provider value={animValue}>
                <GameProvider>
                    <DragProvider>
                        <TutorialHintProvider value={hint}>
                            <TutorialBoard />
                        </TutorialHintProvider>
                    </DragProvider>
                </GameProvider>
            </AnimationContext.Provider>
        </TutPlayContext.Provider>
    );
};

/* ------------------------------- the board --------------------------------- */
const TutorialBoard = () => {
    usePreventScroll();
    const { names, summary, stepIdx, awaiting, finished, beatText, move, skipToEnd, onExit } = useTutPlay();
    const { setSelectedCards } = useGame();
    const { t } = useLocalization();

    // auto-select the scripted cards so the right wooden button appears; keyed
    // on the awaiting step so it doesn't loop on every render.
    const selKey = awaiting ? `${stepIdx}` : 'none';
    useEffect(() => {
        if (move && move.cards.length && (move.kind === REPLAY_STEP.ATTACK || move.kind === REPLAY_STEP.PASS || move.kind === REPLAY_STEP.COVER)) {
            setSelectedCards(move.cards.map((c) => ({ ...c })));
        } else {
            setSelectedCards([]);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [selKey]);

    const moveHint = move
        ? (move.mode === 'drag' ? t('tut_press_or_drag') : (move.action === 'pickup' || move.action === 'good') ? t('tut_press_button') : t('tut_press_or_drag'))
        : '';

    // board inset at top to clear the narration bar
    const boardInset: React.CSSProperties = {
        position: 'absolute',
        top: 'calc(124px + max(8px, env(safe-area-inset-top)))',
        left: 0,
        right: 0,
        bottom: 0,
    };

    return (
        <GameBoard interactive boardInset={boardInset} chrome={<>
            {/* hidden state hook for automated walkthroughs */}
            <div
                data-testid="tut-state"
                data-step={stepIdx}
                data-mode={move ? move.mode : 'none'}
                data-action={move?.action ?? ''}
                data-card={move && move.cards[0] ? `${move.cards[0].suit}-${move.cards[0].value}` : ''}
                data-target={move?.target ? `${move.target.suit}-${move.target.value}` : ''}
                style={{ display: 'none' }}
            />

            {/* narration bar (below the top button row) */}
            <div
                style={{
                    position: 'absolute', top: 'calc(46px + max(8px, env(safe-area-inset-top)))',
                    left: '50%', transform: 'translateX(-50%)', zIndex: 1100, width: 'min(94vw, 560px)',
                    display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6,
                }}
            >
                <div
                    data-testid="tut-narration"
                    className="text-shadow"
                    style={{
                        width: '100%', background: 'rgba(20,16,12,0.74)',
                        border: '1px solid rgba(231,151,67,0.35)', borderRadius: 12, padding: '10px 14px',
                        color: 'var(--color-text-primary)', fontSize: '0.95rem', lineHeight: 1.35,
                        textAlign: 'center', boxShadow: '0 2px 10px rgba(0,0,0,0.45)', minHeight: 44,
                    }}
                >
                    {beatText}
                </div>
                {awaiting && (
                    <div
                        className="text-shadow"
                        style={{
                            background: 'rgba(20,60,30,0.78)', border: '1px solid #2fcf63', borderRadius: 10,
                            padding: '6px 12px', color: '#d6ffe2', fontWeight: 700, fontSize: '0.85rem', textAlign: 'center',
                        }}
                    >
                        ▸ {t('tut_your_move')}: {moveHint}
                    </div>
                )}
            </div>

            {/* top-left: leave */}
            <TexturedSurface as="button" seed={0.2} className="btn-icon btn-icon--left" onClick={onExit} aria-label={t('tut_exit')}>
                <span className="btn-icon__symbol">{'<'}</span>
            </TexturedSurface>

            {/* top-right: skip to end */}
            {!finished && (
                <button
                    onClick={skipToEnd}
                    style={{
                        position: 'absolute', top: 'max(8px, env(safe-area-inset-top))', right: 'max(10px, env(safe-area-inset-right))',
                        zIndex: 1100, padding: '6px 12px', borderRadius: 999, border: '1px solid rgba(255,255,255,0.2)',
                        background: 'rgba(20,16,12,0.7)', color: 'var(--color-text-primary)', fontSize: '0.78rem', fontWeight: 700, cursor: 'pointer',
                    }}
                >
                    {t('tut_skip')} »
                </button>
            )}

            {/* end-of-game card */}
            {finished && (
                <div
                    data-testid="tut-end"
                    style={{ position: 'absolute', inset: 0, zIndex: 1300, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'rgba(8,6,4,0.55)' }}
                >
                    <div
                        className="text-shadow"
                        style={{
                            width: 'min(90vw, 440px)', textAlign: 'center', background: 'rgba(24,18,12,0.92)',
                            border: '1px solid rgba(231,151,67,0.4)', borderRadius: 16, padding: '24px 22px',
                            color: 'var(--color-text-primary)', boxShadow: '0 10px 40px rgba(0,0,0,0.6)',
                        }}
                    >
                        <div style={{ fontSize: '2rem', marginBottom: 6 }}>🃏</div>
                        <p style={{ fontSize: '1.05rem', lineHeight: 1.4, margin: '0 0 6px' }}>{t('tut_fool', { name: names[summary.fool] })}</p>
                        <p style={{ fontSize: '0.9rem', opacity: 0.85, margin: '0 0 18px' }}>{t('tut_done')}</p>
                        <div style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>
                            <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--md" onClick={() => window.location.reload()}>
                                <span className="btn-wood-text">{t('tut_replay')}</span>
                            </TexturedSurface>
                            <TexturedSurface as="button" seed={0.6} className="btn-wood btn-wood--md" onClick={onExit}>
                                <span className="btn-wood-text">{t('tut_exit')}</span>
                            </TexturedSurface>
                        </div>
                    </div>
                </div>
            )}
        </>} />
    );
};

/* ------------------------------- intro card -------------------------------- */
const IntroCard = ({ onStart, onSkip }: { onStart: () => void; onSkip: () => void }) => {
    const { t } = useLocalization();
    const bullets: TutKey[] = ['tut_intro', 'tut_goal', 'tut_deck_low', 'tut_trump', 'tut_drag_tip'];
    return (
        <div style={{ position: 'absolute', inset: 0, zIndex: 1300, display: 'flex', alignItems: 'center', justifyContent: 'center', padding: 16 }}>
            <div
                className="text-shadow"
                style={{
                    width: 'min(92vw, 480px)', background: 'rgba(24,18,12,0.92)', border: '1px solid rgba(231,151,67,0.4)',
                    borderRadius: 16, padding: '24px 22px', color: 'var(--color-text-primary)', boxShadow: '0 10px 40px rgba(0,0,0,0.6)',
                }}
            >
                <h1 style={{ fontSize: '1.5rem', margin: '0 0 4px', textAlign: 'center' }}>{t('tut_title')}</h1>
                <p style={{ fontSize: '0.9rem', opacity: 0.85, textAlign: 'center', margin: '0 0 16px' }}>{t('tut_subtitle')}</p>
                <ul style={{ margin: '0 0 20px', paddingLeft: 20, display: 'flex', flexDirection: 'column', gap: 10 }}>
                    {bullets.map((k) => (<li key={k} style={{ fontSize: '0.92rem', lineHeight: 1.4 }}>{t(k)}</li>))}
                </ul>
                <div style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>
                    <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--md" onClick={onStart} data-testid="tut-start">
                        <span className="btn-wood-text">{t('tut_start')} ▶</span>
                    </TexturedSurface>
                    <TexturedSurface as="button" seed={0.7} className="btn-wood btn-wood--md" onClick={onSkip}>
                        <span className="btn-wood-text">{t('tut_skip')}</span>
                    </TexturedSurface>
                </div>
            </div>
        </div>
    );
};

/* --------------------------------- root ------------------------------------ */
const buildTutorialData = async () => {
    const code = bigintToBytes(bytesToBigint(kernelB32Decode(TUTORIAL_MOVES_CODE)));
    await ensureBotsAsync();
    const summary = replaySummary(code);
    if (!summary) throw new Error('tutorial: the frozen code does not decode');
    const names = TUTORIAL_NAMES.slice(0, summary.numPlayers);
    const withSelf = mkWithSelf(summary.powerSuit);

    // Built for SEAT 0: the learner is a player, not a spectator, so the kernel
    // masks their boards exactly as it would in a real game — they see their own
    // hand and nobody else's. All withSelf adds is the display sort.
    const frames = buildReplayFrames(code, GAME_ID, names, {
        viewer: LEARNER_SEAT, fool: summary.fool,
    }).map((f) => ({
        ...f,
        game: withSelf(f.game),
        seq: {
            ...f.seq,
            game: withSelf(f.seq.game as ReplayGameState),
            events: f.seq.events.map((e) => ({ ...e, game_state: withSelf(e.game_state as ReplayGameState) })),
        },
    }));
    const initial = withSelf(preDealGame(frames[0]));
    return { summary, frames, initial, names };
};

export const Tutorial = () => {
    const router = useRouter();
    const [mounted, setMounted] = useState(false);
    const [started, setStarted] = useState(false);
    useEffect(() => setMounted(true), []);

    // Async: the replay runs in bots.wasm, which the browser must compile
    // asynchronously. undefined = still decoding, null = failed.
    const [data, setData] = useState<Awaited<ReturnType<typeof buildTutorialData>> | null | undefined>(undefined);
    useEffect(() => {
        if (!mounted) return;
        let cancelled = false;
        buildTutorialData()
            .then((d) => { if (!cancelled) setData(d); })
            .catch((e) => {
                console.error('Tutorial decode failed:', e);
                if (!cancelled) setData(null);
            });
        return () => { cancelled = true; };
    }, [mounted]);

    if (!mounted || data === undefined) return null;

    // Nobody signs in to the tutorial: the learner's seat is the board's own
    // (mySeat), and the page names its hand by the seat (state/view.ts seatKey).
    const tutorialAuth = {
        user_id: null, username: 'You', loading: false,
        signIn: async () => ({} as any), signUp: async () => ({} as any),
        signOut: async () => {}, updatePassword: async () => {},
        redirectAfterLogin: null, setRedirectAfterLogin: () => {}, clearRedirectAfterLogin: () => {},
    };
    const exit = () => router.push('/dashboard');

    if (!data) {
        return (
            <div className="game-container"><WoolBackgroundLayer /><IntroCard onStart={exit} onSkip={exit} /></div>
        );
    }

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            <AuthContext.Provider value={tutorialAuth as any}>
                <ReplayServerProvider gameId={GAME_ID} initialGame={data.initial}>
                    <FernFractalProvider>
                        <AnimationProvider>
                            {!started ? (
                                <IntroCard onStart={() => setStarted(true)} onSkip={exit} />
                            ) : (
                                <TutorialPlayback
                                    summary={data.summary}
                                    frames={data.frames}
                                    names={data.names}
                                    onExit={exit}
                                />
                            )}
                        </AnimationProvider>
                    </FernFractalProvider>
                </ReplayServerProvider>
            </AuthContext.Provider>
        </div>
    );
};
