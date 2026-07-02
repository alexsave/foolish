'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { Leaderboard } from '../../components/Leaderboard';

// Public like /about: user_elo_ratings and bots are world-readable, so the
// standings don't need a session. Signed-out visitors just get no highlight.
export default function LeaderboardPage() {
  return (
    <ErrorBoundary context="Leaderboard Page">
      <Leaderboard />
    </ErrorBoundary>
  );
}
