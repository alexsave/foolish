'use client';

import { ErrorBoundary } from '../components/ErrorBoundary';
import { UnprotectedRoute } from '../components/UnprotectedRoute';
import { Welcome } from '../components/Welcome';

export default function HomePage() {
  return (
    <ErrorBoundary context="Welcome Page">
      <UnprotectedRoute>
        <Welcome />
      </UnprotectedRoute>
    </ErrorBoundary>
  );
}
