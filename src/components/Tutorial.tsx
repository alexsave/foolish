'use client';

import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { useRouter } from 'next/navigation';
import { Card, LOG_TYPE, LogType } from '@shared/types.ts';
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
import { codeToGame } from '@shared/replay/codec.ts';
import { decodeReplay } from '@shared/replay/decode.ts';
import { DecodedReplay } from '@shared/replay/core.ts';
import { buildReplaySteps, stepToGame, ReplayStep, ReplayGameState } from '../replay/view';
import { buildReplaySequences, preDealGame } from '../replay/animate';
import { canCoverCards } from '../utils/gameValidation';
import { tutorialStrings, tfmt, TutKey } from '../localization/tutorialStrings';
import { TUTORIAL_MOVES_CODE, TUTORIAL_NAMES } from './tutorialGame';

const SELF_ID = 'seat-0';
const GAME_ID = 'tutorial';
const RESULT = { game_id: GAME_ID };

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;
const LEARNER_KINDS: LogType[] = [LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS, LOG_TYPE.PICKUP, LOG_TYPE.GOOD];

/* Give every committed game state a real `self` (seat 0's face-up hand, in a
 * stable display order) so the live hand + action buttons render. */
function deriveSelf(state: ReplayGameState, powerSuit: number) {
    const hand = (state.replay_hands?.[0] ?? []).filter((c): c is Card => !!c);
    hand.sort((a, b) => {
        const ta = a.suit === powerSuit ? 1 : 0, tb = b.suit === powerSuit ? 1 : 0;
        return ta - tb || a.value - b.value || a.suit - b.suit;
    });
    const p0 = state.players?.[0];
    return {
        player_id: SELF_ID,
        name: p0?.name ?? 'You',
        status: p0?.status ?? 'in',
        hand_length: p0?.hand_length ?? hand.length,
        is_ai: false,
        hand,
        awaiting_attack: false,
        strategy_key: 'human',
    };
}
const mkWithSelf = (powerSuit: number) => <T extends ReplayGameState>(state: T): T =>
    (!state || !state.players ? state : ({ ...state, self: deriveSelf(state, powerSuit) } as T));

/* ----------------------------- concept beats ------------------------------- */
interface Beat { at: number; key: TutKey; extra?: TutKey; name?: string; }

function buildBeats(steps: ReplayStep[], decoded: DecodedReplay, names: string[]): Beat[] {
    const beats: Beat[] = [];
    const seen = new Set<string>();
    const once = (k: string) => (seen.has(k) ? false : (seen.add(k), true));
    const ps = decoded.powerSuit;
    const fa = decoded.firstAttacker;
    beats.push({ at: 0, key: fa === 0 ? 'first_attacker_you' : 'first_attacker', name: names[fa] });

    for (let i = 0; i < steps.length; i++) {
        const s = steps[i];
        const prev = steps[i - 1];
        switch (s.kind) {
            case LOG_TYPE.ATTACK:
                if (prev && prev.battles.length > 0 && once('throwIn'))
                    beats.push({ at: i, key: 'throw_in', extra: 'capacity' });
                break;
            case LOG_TYPE.COVER: {
                if (once('cover')) beats.push({ at: i, key: 'cover', extra: 'stack_rule' });
                const cov = s.cards[0], tgt = s.target;
                if (cov && tgt && cov.suit === ps && tgt.suit !== ps && once('trumpCover'))
                    beats.push({ at: i, key: 'trump_cover' });
                break;
            }
            case LOG_TYPE.PASS: if (once('pass')) beats.push({ at: i, key: 'pass' }); break;
            case LOG_TYPE.PICKUP: if (once('pickup')) beats.push({ at: i, key: 'pickup' }); break;
            case LOG_TYPE.GOOD: if (once('good')) beats.push({ at: i, key: 'good' }); break;
            case LOG_TYPE.DISCARD: if (once('discard')) beats.push({ at: i, key: 'discard' }); break;
            case LOG_TYPE.DRAW: if (once('draw')) beats.push({ at: i, key: 'draw' }); break;
            case LOG_TYPE.PLAYER_OUT: if (once('out')) beats.push({ at: i, key: 'out', name: names[s.seat ?? 0] }); break;
            case 'end': beats.push({ at: i, key: 'fool', name: names[decoded.fool] }); break;
        }
        if (s.deckCount === 0 && s.flipped === null && once('deckEmpty')) beats.push({ at: i, key: 'deck_empty' });
    }
    beats.sort((a, b) => a.at - b.at);
    return beats;
}

/* The learner's pending move, derived from the next scripted step. */
interface Move {
    kind: LogType;
    cards: Card[];          // cards to highlight in hand
    target: Card | null;    // attack card to cover (COVER)
    action: string | null;  // wooden button to glow, or null when a drag is needed
    mode: 'button' | 'drag';
}

/* ------------------------- playback (state + override) --------------------- */
interface TutPlay {
    S: Record<TutKey, string>;
    names: string[];
    decoded: DecodedReplay;
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
    decoded: DecodedReplay;
    steps: ReplayStep[];
    sequences: AnimationSequenceMessage[];
    names: string[];
    onExit: () => void;
}

const TutorialPlayback = ({ decoded, steps, sequences, names, onExit }: PlaybackProps) => {
    const { updateGameState, game } = useServer();
    const real = useAnimation();
    const { isAnimating } = real;
    const { language } = useLocalization();
    const S = tutorialStrings[language];
    const withSelf = useMemo(() => mkWithSelf(decoded.powerSuit), [decoded.powerSuit]);

    const [stepIdx, setStepIdx] = useState(-1);
    const stepRef = useRef(stepIdx);
    stepRef.current = stepIdx;
    const lastIdx = steps.length - 1;
    const publishSeq = useRef(0);

    const beats = useMemo(() => buildBeats(steps, decoded, names), [steps, decoded, names]);
    const isLearnerStep = useCallback(
        (i: number) => i >= 0 && i <= lastIdx && steps[i].seat === 0 && LEARNER_KINDS.includes(steps[i].kind as LogType),
        [steps, lastIdx],
    );

    const publishStep = useCallback((i: number) => {
        const seq: AnimationSequenceMessage = JSON.parse(JSON.stringify(sequences[i]));
        seq.sequence_id = `tut-${i}-${++publishSeq.current}-${Math.random().toString(36).slice(2)}`;
        seq.timestamp = Date.now();
        if (seq.events[0]) (seq.events[0] as any)._nonce = seq.sequence_id;
        animationFeed.publish(seq);
        setStepIdx(i);
    }, [sequences]);

    // performing a (correct) action advances the scripted game
    const tryAdvance = useCallback((kind: LogType): boolean => {
        const next = stepRef.current + 1;
        if (next <= lastIdx && steps[next].seat === 0 && steps[next].kind === kind) {
            publishStep(next);
            return true;
        }
        return false;
    }, [steps, lastIdx, publishStep]);

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
        updateGameState(GAME_ID, withSelf(stepToGame(decoded, steps[lastIdx], GAME_ID, names) as ReplayGameState));
        setStepIdx(lastIdx);
    }, [decoded, steps, names, lastIdx, real, updateGameState, withSelf]);

    const awaiting = stepIdx >= 0 && stepIdx < lastIdx && !isAnimating && isLearnerStep(stepIdx + 1);
    const pending = awaiting ? steps[stepIdx + 1] : null;
    const finished = stepIdx >= lastIdx;

    // derive the learner's move (which cards/button, or a drag) from the step
    const move: Move | null = useMemo(() => {
        if (!pending) return null;
        const k = pending.kind as LogType;
        if (k === LOG_TYPE.ATTACK || k === LOG_TYPE.PASS)
            return { kind: k, cards: pending.cards, target: null, action: k === LOG_TYPE.ATTACK ? 'attack' : 'pass', mode: 'button' };
        if (k === LOG_TYPE.PICKUP)
            return { kind: k, cards: [], target: null, action: 'pickup', mode: 'button' };
        if (k === LOG_TYPE.GOOD)
            return { kind: k, cards: [], target: null, action: 'good', mode: 'button' };
        if (k === LOG_TYPE.COVER) {
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
        ? tfmt(S[activeBeat.key], activeBeat.name ? { name: activeBeat.name } : undefined) +
          (activeBeat.extra ? ' ' + S[activeBeat.extra] : '')
        : '';

    const hint: TutorialHint | null = move
        ? { cards: move.cards, action: move.action, targetCard: move.target }
        : null;

    const animValue = useMemo(() => ({
        ...real,
        attack: async () => { tryAdvance(LOG_TYPE.ATTACK); return RESULT; },
        pass: async () => { tryAdvance(LOG_TYPE.PASS); return RESULT; },
        pickup: async () => { tryAdvance(LOG_TYPE.PICKUP); return RESULT; },
        cover: async () => { tryAdvance(LOG_TYPE.COVER); return RESULT; },
        good: async () => { tryAdvance(LOG_TYPE.GOOD); return RESULT; },
    }), [real, tryAdvance]);

    const play: TutPlay = { S, names, decoded, stepIdx, awaiting, finished, beatText, move, skipToEnd, onExit };

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
    const { S, names, decoded, stepIdx, awaiting, finished, beatText, move, skipToEnd, onExit } = useTutPlay();
    const { setSelectedCards } = useGame();

    // auto-select the scripted cards so the right wooden button appears; keyed
    // on the awaiting step so it doesn't loop on every render.
    const selKey = awaiting ? `${stepIdx}` : 'none';
    useEffect(() => {
        if (move && move.cards.length && (move.kind === LOG_TYPE.ATTACK || move.kind === LOG_TYPE.PASS || move.kind === LOG_TYPE.COVER)) {
            setSelectedCards(move.cards.map((c) => ({ ...c })));
        } else {
            setSelectedCards([]);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [selKey]);

    const moveHint = move
        ? (move.mode === 'drag' ? S.press_or_drag : (move.action === 'pickup' || move.action === 'good') ? S.press_button : S.press_or_drag)
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
                        ▸ {S.your_move}: {moveHint}
                    </div>
                )}
            </div>

            {/* top-left: leave */}
            <TexturedSurface as="button" seed={0.2} className="btn-icon btn-icon--left" onClick={onExit} aria-label={S.exit}>
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
                    {S.skip} »
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
                        <p style={{ fontSize: '1.05rem', lineHeight: 1.4, margin: '0 0 6px' }}>{tfmt(S.fool, { name: names[decoded.fool] })}</p>
                        <p style={{ fontSize: '0.9rem', opacity: 0.85, margin: '0 0 18px' }}>{S.done}</p>
                        <div style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>
                            <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--md" onClick={() => window.location.reload()}>
                                <span className="btn-wood-text">{S.replay}</span>
                            </TexturedSurface>
                            <TexturedSurface as="button" seed={0.6} className="btn-wood btn-wood--md" onClick={onExit}>
                                <span className="btn-wood-text">{S.exit}</span>
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
    const { language } = useLocalization();
    const S = tutorialStrings[language];
    const bullets: TutKey[] = ['intro', 'goal', 'deck_low', 'trump', 'drag_tip'];
    return (
        <div style={{ position: 'absolute', inset: 0, zIndex: 1300, display: 'flex', alignItems: 'center', justifyContent: 'center', padding: 16 }}>
            <div
                className="text-shadow"
                style={{
                    width: 'min(92vw, 480px)', background: 'rgba(24,18,12,0.92)', border: '1px solid rgba(231,151,67,0.4)',
                    borderRadius: 16, padding: '24px 22px', color: 'var(--color-text-primary)', boxShadow: '0 10px 40px rgba(0,0,0,0.6)',
                }}
            >
                <h1 style={{ fontSize: '1.5rem', margin: '0 0 4px', textAlign: 'center' }}>{S.title}</h1>
                <p style={{ fontSize: '0.9rem', opacity: 0.85, textAlign: 'center', margin: '0 0 16px' }}>{S.subtitle}</p>
                <ul style={{ margin: '0 0 20px', paddingLeft: 20, display: 'flex', flexDirection: 'column', gap: 10 }}>
                    {bullets.map((k) => (<li key={k} style={{ fontSize: '0.92rem', lineHeight: 1.4 }}>{S[k]}</li>))}
                </ul>
                <div style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>
                    <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--md" onClick={onStart} data-testid="tut-start">
                        <span className="btn-wood-text">{S.start} ▶</span>
                    </TexturedSurface>
                    <TexturedSurface as="button" seed={0.7} className="btn-wood btn-wood--md" onClick={onSkip}>
                        <span className="btn-wood-text">{S.skip}</span>
                    </TexturedSurface>
                </div>
            </div>
        </div>
    );
};

/* --------------------------------- root ------------------------------------ */
export const Tutorial = () => {
    const router = useRouter();
    const [mounted, setMounted] = useState(false);
    const [started, setStarted] = useState(false);
    useEffect(() => setMounted(true), []);

    const data = useMemo(() => {
        if (!mounted) return null;
        try {
            const decoded = decodeReplay(codeToGame(TUTORIAL_MOVES_CODE));
            const steps = buildReplaySteps(decoded);
            const names = TUTORIAL_NAMES.slice(0, decoded.playerCount);
            const withSelf = mkWithSelf(decoded.powerSuit);
            const sequences = buildReplaySequences(decoded, steps, GAME_ID, names).map((seq) => ({
                ...seq,
                game: withSelf(seq.game as ReplayGameState),
                events: seq.events.map((e) => ({ ...e, game_state: withSelf(e.game_state as ReplayGameState) })),
            }));
            const initial = withSelf(preDealGame(decoded, steps[0], GAME_ID, names) as ReplayGameState);
            return { decoded, steps, sequences, initial, names };
        } catch (e) {
            console.error('Tutorial decode failed:', e);
            return null;
        }
    }, [mounted]);

    if (!mounted) return null;

    const tutorialAuth = {
        user_id: SELF_ID, username: 'You', loading: false,
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
                                    decoded={data.decoded}
                                    steps={data.steps}
                                    sequences={data.sequences}
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
