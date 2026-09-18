'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { MatchHistory } from '../../components/MatchHistory';

export default function HistoryPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Match History Page">
        <ProtectedRoute>
          <MatchHistory />
        </ProtectedRoute>
      </ErrorBoundary>
    </KernelGate>
  );
}
