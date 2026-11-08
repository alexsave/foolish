import dynamic from 'next/dynamic';
import { perStrokeOf, tunerPlay } from '../../audio/tunerPlay';

// THE TUNER ON THE REPLAY SCREEN, in development only (Alex 2026-11-08: "temporarily
// swap out the oracle overlay for a sound tuner overlay"): ReplayStage opens this
// from the Oracle's button while TUNER_IN_REPLAY is on. It is SoundTuner.tsx's
// compact form beside a game, with "Again" to hear the move on the board once more.
// The Oracle itself is untouched; turning the flag off brings it back.

/** The one switch: a constant in a production build, so nothing of this ships. */
export const TUNER_IN_REPLAY = process.env.NODE_ENV === 'development';

const SoundTuner = TUNER_IN_REPLAY ? dynamic(() => import('../SoundTuner'), { ssr: false }) : null;

/** The replay's step, kept for this tab across the reload a rebuild ends in. */
const STEP_KEY = 'foolish_tuner_replay_step';
export const keepStep = (step: number): void => {
    try { sessionStorage.setItem(STEP_KEY, `${location.pathname} ${step}`); } catch { /* private mode */ }
};
/** The step to come back to on this page; null when the tuner did not reload it. It is
 *  forgotten a few seconds after a load, not at once: the tuner may reload again while
 *  the dev server catches up (SoundTuner's pending check), and that load needs it too. */
export const takeStep = (): number | null => {
    try {
        const v = sessionStorage.getItem(STEP_KEY);
        setTimeout(() => { try { if (sessionStorage.getItem(STEP_KEY) === v) sessionStorage.removeItem(STEP_KEY); } catch { /* */ } }, 4000);
        const m = v ? /^(\S+) (-?\d+)$/.exec(v) : null;
        return m && m[1] === location.pathname ? Number(m[2]) : null;
    } catch { return null; }
};

const btn: React.CSSProperties = {
    background: '#3a2c22', color: '#f5e6c8', border: '1px solid #6b5443', borderRadius: 6,
    padding: '2px 8px', font: 'inherit', fontSize: 12, cursor: 'pointer',
};

export const TunerOverlay = ({ stepIdx, again, back, forward, onClose }: {
    stepIdx: number; again: () => void; back: () => void; forward: () => void; onClose: () => void;
}) => {
    if (!SoundTuner) return null;
    return (
        <div
            data-testid="tuner-overlay"
            // Keys typed into the tuner's fields are not the replay's transport keys.
            onKeyDown={(e) => e.stopPropagation()}
            style={{
                position: 'absolute',
                top: 'calc(44px + max(8px, env(safe-area-inset-top)))',
                right: 'max(10px, env(safe-area-inset-right))',
                zIndex: 1200,
                width: 'min(94vw, 470px)',
                maxHeight: 'calc(100dvh - 190px)',
                display: 'flex', flexDirection: 'column',
                color: '#f5e6c8', background: '#1c1410', borderRadius: 10,
                border: '1px solid #6b5443', boxShadow: '0 6px 24px rgba(0,0,0,0.45)',
            }}
        >
            <div style={{ display: 'flex', gap: 6, alignItems: 'center', padding: '6px 8px', borderBottom: '1px solid #4a382c' }}>
                <strong style={{ fontSize: 13, marginRight: 'auto' }}>Sound tuner</strong>
                <button type="button" style={btn} onClick={back} title="step back">◀</button>
                <button type="button" style={{ ...btn, background: '#5b7a3a', borderColor: '#7d9c58' }} onClick={again}
                        disabled={stepIdx < 0} title="play this move on the board again">
                    Again
                </button>
                <button type="button" style={btn} onClick={forward} title="next move">▶</button>
                <button type="button" style={btn} onClick={onClose} aria-label="close">×</button>
            </div>
            <div style={{ overflowY: 'auto', padding: '6px 8px 8px' }} data-chat-scrollable>
                <SoundTuner play={tunerPlay} perStroke={perStrokeOf} compact beforeReload={() => keepStep(stepIdx)} />
            </div>
        </div>
    );
};
