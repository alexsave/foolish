'use client';

import React, { useEffect, useRef, useState } from 'react';
import { AuthContext } from '../contexts/AuthContext';
import { useLocalization } from '../contexts/LocalizationContext';
import { useServer } from '../contexts/ServerContext';
import { playLocally } from '../common/backend';
import type { Promo } from '../common/promo';
import { GAME_STATUS } from '../state/view';
import { usePreventScroll } from '../utils/usePreventScroll';
import { ProtectedRoute } from './ProtectedRoute';
import { GameBoard } from './GameBoard';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { TexturedSurface } from './TexturedSurface';
import { WinScreen } from './WinScreen';

/* A promo code's game (src/common/promo.ts): the visitor against the bot, dealt
 * the moment the page opens, played in this tab by the kernel's local table
 * (backend.ts playLocally). Nobody signs in: the seat is the board's own
 * (mySeat), so the identity below only has to say that someone is here. */
const LOCAL_AUTH = {
    user_id: 'local', username: null, loading: false, redirectAfterLogin: null,
    setRedirectAfterLogin: () => {}, clearRedirectAfterLogin: () => {},
};

export const PromoGame = ({ promo }: { promo: Promo }) => {
    const { t, language, loaded, visitLanguage } = useLocalization();
    const [playing, setPlaying] = useState(false);
    // eslint-disable-next-line react-hooks/exhaustive-deps
    useEffect(() => visitLanguage(promo.lang), [promo]);
    // The table opens once the code's language is in, so the visitor's seat is
    // named in it ("Гость" on /br, not the English standing in while it loads):
    // the name the replay's QR carries too.
    // Opened ONCE, and left only when the page goes: the language settling again
    // afterwards (a stored choice arriving) must not deal a second game under the
    // page that is already showing the first.
    const inLanguage = language === promo.lang && loaded;
    const stop = useRef<(() => void) | null>(null);
    useEffect(() => {
        if (!inLanguage || stop.current) return;
        stop.current = playLocally(promo.code, t('visitor'));
        setPlaying(true);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [inLanguage]);
    useEffect(() => () => { stop.current?.(); stop.current = null; }, []);
    if (!playing) return null;
    return (
        <AuthContext.Provider value={LOCAL_AUTH}>
            <ProtectedRoute>
                <PromoBoard />
            </ProtectedRoute>
        </AuthContext.Provider>
    );
};

const PromoBoard = () => {
    const { view } = useServer();
    const { t } = useLocalization();
    usePreventScroll();
    if (!view) return null;
    // The finish screen is the online game's own (ranks, places, the replay's QR
    // and link), unrated as a local game is, with this page's two ways on.
    if (view.status === GAME_STATUS.GAME_OVER) {
        return (
            <div data-promo-game={view.status} style={{ display: 'contents' }}>
                <WinScreen title={t('game_over')} actions={
                    <div data-testid="promo-end" style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>
                        <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--lg" onClick={() => window.location.reload()}>
                            <span className="btn-wood-text">{t('play_again')}</span>
                        </TexturedSurface>
                        {/* A full navigation, so the local game is left behind with the page. */}
                        <TexturedSurface as="button" seed={0.6} className="btn-wood btn-wood--lg" onClick={() => window.location.assign('/')}>
                            <span className="btn-wood-text">{t('register')}</span>
                        </TexturedSurface>
                    </div>
                } />
            </div>
        );
    }
    return (
        <div data-game-container className="game-container" data-promo-game={view.status}>
            <WoolBackgroundLayer />
            <GameBoard interactive />
        </div>
    );
};
