'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { Leaderboard } from '../../components/Leaderboard';

// Public like /about: the standings need no session, and signed-out visitors
// just get no highlight. The kernel's client reads them, so the board waits for
// the kernel (components/KernelGate.tsx).
export default function LeaderboardPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Leaderboard Page">
        <Leaderboard />
      </ErrorBoundary>
    </KernelGate>
  );
}
