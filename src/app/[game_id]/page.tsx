'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { GameView } from '../../components/GameView';

export default function GamePage() {
  return (
    <ErrorBoundary context="Game Page">
      <ProtectedRoute>
        <GameView />
      </ProtectedRoute>
    </ErrorBoundary>
  );
}
