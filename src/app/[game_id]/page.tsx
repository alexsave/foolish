'use client';

import { useParams } from 'next/navigation';
import { ErrorBoundary } from '../../components/ErrorBoundary';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { GameView } from '../../components/GameView';
import { ReplayScreen } from '../../components/ReplayScreen';
import { classifyPathSegment } from '../../replay/codec';

export default function GamePage() {
  const segment = useParams<{ game_id: string }>().game_id || '';

  // Long path segments are self-contained replay payloads: the base32 string
  // IS the whole game, so the viewer needs no auth and no database row.
  // Short segments stay the legacy authenticated live-game codes.
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
