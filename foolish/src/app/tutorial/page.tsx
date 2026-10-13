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
        <SoundButton />
      </ErrorBoundary>
    </KernelGate>
  );
}
