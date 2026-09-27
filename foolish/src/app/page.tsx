'use client';

import { ErrorBoundary } from '../components/ErrorBoundary';
import { KernelGate } from '../components/KernelGate';
import { UnprotectedRoute } from '../components/UnprotectedRoute';
import { Welcome } from '../components/Welcome';

export default function HomePage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Welcome Page">
        <UnprotectedRoute>
          <Welcome />
        </UnprotectedRoute>
      </ErrorBoundary>
    </KernelGate>
  );
}
