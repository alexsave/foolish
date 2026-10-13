'use client';

import { useParams } from 'next/navigation';
import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { GameView } from '../../components/GameView';
import { ReplayScreen } from '../../components/ReplayScreen';
import { SoundButton } from '../../components/SoundButton';
import { classifyPathSegment } from '@api/common/replay_codec.ts';

export default function GamePage() {
  const segment = useParams<{ game_id: string }>().game_id || '';

  // Long path segments are self-contained replay payloads: the base32 string
  // IS the whole game, so the viewer needs no auth and no database row.
  // Short segments stay the legacy authenticated live-game codes.
  // classifyPathSegment is a string-length test and touches no kernel, so it is
  // safe to read the segment before the gate opens.
  if (classifyPathSegment(segment) === 'replay') {
    return (
      <KernelGate>
        <ErrorBoundary context="Replay Page">
          <ReplayScreen code={segment} />
          <SoundButton />
        </ErrorBoundary>
      </KernelGate>
    );
  }

  return (
    <KernelGate>
      <ErrorBoundary context="Game Page">
        <ProtectedRoute>
          <GameView />
        </ProtectedRoute>
        {/* The sound control belongs to the ROUTE, not to the board: a game is
            heard from its lobby (the deal) to its results (the finish), and
            those are three different screens under this one page. */}
        <SoundButton />
      </ErrorBoundary>
    </KernelGate>
  );
}
