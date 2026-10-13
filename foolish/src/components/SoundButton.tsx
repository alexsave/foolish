import { useSyncExternalStore } from 'react';
import { TexturedSurface } from './TexturedSurface';
import { cueAudio } from '../audio/cueAudio';

// The sound control: one tap mutes or unmutes, and the choice is remembered in
// this browser (src/audio/cueAudio.ts). The same little wood square as the back
// button, in the opposite corner.
//
// MOUNTED BY THE ROUTE (src/app/[game_id], src/app/tutorial), beside the screen
// and not inside the board, so it is there for the lobby, the board and the
// results alike. That is why it is `fixed`: the icon buttons place themselves
// against whatever positioned box holds them, and a route has none.
//
// WHICH CORNER IS THE ROUTE'S TO SAY, because the corners are not free on every
// screen: the tutorial keeps its Skip pill top-right, so there the button takes
// the second slot on the left, next to the way out (`beside="back"`).
//
// THE TAP IS ALSO WHAT STARTS AUDIO for a player who arrived muted: a browser
// only lets a page make sound from inside a user gesture, and `setMuted(false)`
// is called from one here.
//
// The label is English and not a translated string, like the sign-out button's:
// a key in c/i18n has to arrive in all twenty-five languages at once
// (docs/AUDIO.md lists it as open).
/** The second slot of the top-left corner: one icon button and a gap along. */
const BESIDE_BACK: React.CSSProperties = {
    position: 'fixed',
    left: 'calc(max(10px, env(safe-area-inset-left, 0px)) + var(--btn-icon-size) + 8px)',
};

export const SoundButton = ({ beside }: { beside?: 'back' }) => {
    const { muted } = useSyncExternalStore(cueAudio.subscribe, cueAudio.getState, cueAudio.getServerState);
    const label = muted ? 'Turn sound on' : 'Turn sound off';
    return (
        <TexturedSurface
            as="button"
            seed={0.7}
            className={beside === 'back' ? 'btn-icon' : 'btn-icon btn-icon--right'}
            style={beside === 'back' ? BESIDE_BACK : { position: 'fixed' }}
            onClick={() => cueAudio.setMuted(!muted)}
            aria-label={label}
            aria-pressed={!muted}
            title={label}
            data-sound={muted ? 'off' : 'on'}
        >
            <svg className="btn-icon__symbol" width="26" height="26" viewBox="0 0 24 24" aria-hidden="true">
                {/* the speaker: a box and its horn */}
                <path d="M3 9h4l5-4v14l-5-4H3z" fill="currentColor" />
                {muted ? (
                    <path d="M15.5 9.5l5 5m0-5l-5 5" stroke="currentColor" strokeWidth="2.2" strokeLinecap="square" fill="none" />
                ) : (
                    <>
                        <path d="M15 9.5a3.5 3.5 0 0 1 0 5" stroke="currentColor" strokeWidth="2.2" fill="none" />
                        <path d="M17.5 6.5a7.5 7.5 0 0 1 0 11" stroke="currentColor" strokeWidth="2.2" fill="none" />
                    </>
                )}
            </svg>
        </TexturedSurface>
    );
};
