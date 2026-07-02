'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { MatchHistory } from '../../components/MatchHistory';

export default function HistoryPage() {
  return (
    <ErrorBoundary context="Match History Page">
      <ProtectedRoute>
        <MatchHistory />
      </ProtectedRoute>
    </ErrorBoundary>
  );
}
