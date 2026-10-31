'use client';

import React, { useEffect, useState } from 'react';
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
import { EndCard } from './EndCard';

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
    // named in it ("Вы" on /br, not the English standing in while it loads).
    const inLanguage = language === promo.lang && loaded;
    useEffect(() => {
        if (!inLanguage) return;
        const stop = playLocally(promo.code, t('you'));
        setPlaying(true);
        return stop;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [promo, inLanguage]);
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
    const over = view.status === GAME_STATUS.GAME_OVER;
    return (
        <div data-game-container className="game-container" data-promo-game={view.status}>
            <WoolBackgroundLayer />
            <GameBoard interactive chrome={over && (
                <EndCard testId="promo-end" actions={<>
                    <TexturedSurface as="button" seed={0.3} className="btn-wood btn-wood--md" onClick={() => window.location.reload()}>
                        <span className="btn-wood-text">{t('rematch')}</span>
                    </TexturedSurface>
                    {/* A full navigation, so the local game is left behind with the page. A
                        button like its neighbour, not a link: an <a> sits its label lower. */}
                    <TexturedSurface as="button" seed={0.6} className="btn-wood btn-wood--md" onClick={() => window.location.assign('/')}>
                        <span className="btn-wood-text">{t('sign_up')}</span>
                    </TexturedSurface>
                </>}>
                    <p style={{ fontSize: '1.15rem', lineHeight: 1.4, margin: '0 0 18px' }}>
                        {t(view.fool === view.mySeat ? 'you_lose' : 'you_win')}
                    </p>
                </EndCard>
            )} />
        </div>
    );
};
