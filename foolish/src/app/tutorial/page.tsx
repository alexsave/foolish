'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { Tutorial } from '../../components/Tutorial';
import { SoundButton } from '../../components/SoundButton';

export default function TutorialPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Tutorial Page">
        <Tutorial />
        {/* Beside the way out: the tutorial's Skip pill has the other corner. */}
        <SoundButton beside="back" />
      </ErrorBoundary>
    </KernelGate>
  );
}
