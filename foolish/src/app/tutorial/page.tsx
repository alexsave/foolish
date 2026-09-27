'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { Tutorial } from '../../components/Tutorial';

export default function TutorialPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Tutorial Page">
        <Tutorial />
      </ErrorBoundary>
    </KernelGate>
  );
}
