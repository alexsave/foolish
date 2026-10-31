'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { GameView } from '../../components/GameView';
import { ReplayScreen } from '../../components/ReplayScreen';
import { classifyPathSegment } from '@api/common/replay_codec.ts';
import { useUrlGameId } from '../../utils/useUrlGameId';
import { promoOf } from '../../common/promo';
import { PromoGame } from '../../components/PromoGame';

export function GameRoute() {
  const segment = useUrlGameId();

  // The static shell's first render: the address bar is read after mount.
  if (segment === undefined) return null;

  // A promo code (the promo playing cards' QR, src/common/promo.ts) is a game
  // against a bot in this tab: no server, no sign-in. Never a game id: a code
  // carries a letter no id can (c/promo/promo_codes.h).
  const promo = promoOf(segment);
  if (promo) {
    return (
      <ErrorBoundary context="Promo Game">
        <PromoGame promo={promo} />
      </ErrorBoundary>
    );
  }

  // Long path segments are self-contained replay payloads: the base32 string
  // IS the whole game, so the viewer needs no auth and no database row.
  // Short segments stay the legacy authenticated live-game codes.
  // classifyPathSegment is a string-length test and touches no kernel, so it is
  // safe to read the segment before the gate opens.
  if (classifyPathSegment(segment) === 'replay') {
    return (
      <ErrorBoundary context="Replay Page">
        <ReplayScreen code={segment} />
      </ErrorBoundary>
    );
  }

  return (
    <ErrorBoundary context="Game Page">
      <ProtectedRoute>
        <GameView />
      </ProtectedRoute>
    </ErrorBoundary>
  );
}
