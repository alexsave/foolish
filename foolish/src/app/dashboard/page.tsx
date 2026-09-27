'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { ProtectedRoute } from '../../components/ProtectedRoute';
import { Dashboard } from '../../components/Dashboard';

export default function DashboardPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Dashboard Page">
        <ProtectedRoute>
          <Dashboard />
        </ProtectedRoute>
      </ErrorBoundary>
    </KernelGate>
  );
}
