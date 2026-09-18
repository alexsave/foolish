import React, { useCallback, useState } from 'react';
import { useRouter } from 'next/navigation';
import { TexturedSurface } from '../TexturedSurface';
import { useLocalization } from '../../contexts/LocalizationContext';
import { GameBoard } from '../GameBoard';
import { Telestrator } from '../Telestrator';
import { usePreventScroll } from '../../hooks/usePreventScroll';
import { AnimationSequenceMessage } from '../../state/animationFeed';
import { type ReplayFrame } from '../../replay/frames';
import { usePlayback } from '../../replay/usePlayback';
import { useOracle } from '../../replay/useOracle';
import { fmtDuration } from '../../replay/speeds';
import { StepMessage, FoolMessage } from './StepMessage';
import { RevealedHands } from './RevealedHands';
import { OracleOverlay } from '../OracleOverlay';
import {
    IconBoutNext, IconBoutStart, IconEye, IconOracle, IconPause, IconPen,
    IconPlay, IconStepBack, IconStepForward,
} from './ReplayIcons';

export interface StageProps {
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

/** The replay's screen: the board, the readouts and the transport deck. The
 *  deck head itself is usePlayback's and the Oracle's panel is useOracle's. */
export const ReplayStage = ({ fool, code, frames, reverses, gameId, names, times }: StageProps) => {
    usePreventScroll();
    const { t } = useLocalization();
    const router = useRouter();

    const [reveal, setReveal] = useState(false);
    // Telestrator toggle: press to enter a red-pen overlay; press again to exit
    // AND clear it. Because the canvas is unmounted while `drawing` is false,
    // toggling off wipes the strokes and toggling on always starts blank.
    const [drawing, setDrawing] = useState(false);
    const toggleDrawing = useCallback(() => setDrawing((d) => !d), []);

    const play = usePlayback(frames, reverses, times, { drawing, toggleDrawing });
    const oracle = useOracle(frames, code, play.stepIdx, play.playing, gameId);

    const { stepIdx, lastIdx, playing, waitTarget, now } = play;
    const speed = play.speeds[play.speedIdx % play.speeds.length];

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
                    {stepIdx >= 0 && <StepMessage frame={play.frame} names={names} />}
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
                    onChange={(e) => play.jumpTo(Number(e.target.value))}
                    style={{ width: '100%' }}
                />
                <div style={{ display: 'flex', gap: 7, alignItems: 'center', justifyContent: 'flex-end', flexWrap: 'wrap' }}>
                    {btn(<IconBoutStart />, play.boutStart, t('replay_bout_start'))}
                    {btn(<IconStepBack />, play.stepBack, t('replay_step_back'))}
                    {btn(playing ? <IconPause /> : <IconPlay />, () => play.setPlaying((p) => !p), t(playing ? 'pause' : 'replay_play'))}
                    {btn(<IconStepForward />, play.stepForward, t('replay_step_forward'))}
                    {btn(<IconBoutNext />, play.nextBout, t('replay_bout_next'))}
                    {btn(<IconEye />, () => setReveal((r) => !r), t(reveal ? 'hide_cards' : 'reveal_cards'), reveal)}
                    {btn(<IconPen />, toggleDrawing, t(drawing ? 'replay_draw_clear' : 'replay_draw'), drawing)}
                    <span data-testid="oracle-btn-wrap" style={{ opacity: oracle.decision != null ? 1 : 0.4 }}>
                        {btn(
                            <IconOracle />,
                            () => { if (oracle.decision != null) oracle.setOpen((o) => !o); },
                            oracle.decision != null ? t('oracle_button_title') : t('oracle_no_decision'),
                            oracle.open,
                        )}
                    </span>
                    {play.speeds.length > 1 &&
                        btn(speed.label, play.nextSpeed, t('playback_speed'), speed.mult !== null)}
                </div>
            </div>

            {/* Infinite Oracle panel - right-anchored, mounted in the board
                chrome so its mini-cards render inside the replay provider tree */}
            {oracle.open && (
                <OracleOverlay
                    snapshot={oracle.snapshot}
                    onClose={() => oracle.setOpen(false)}
                    onToggleMemory={oracle.toggleMemory}
                    onRetry={oracle.retry}
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
