'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { Dashboard } from '../../components/Dashboard';

export default function DashboardPage() {
  return (
    <ErrorBoundary context="Dashboard Page">
      <ProtectedRoute>
        <Dashboard />
      </ProtectedRoute>
    </ErrorBoundary>
  );
}
