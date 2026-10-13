'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { KernelGate } from '../../components/KernelGate';
import { SoundAudition } from '../../components/SoundAudition';

// The sound workbench (docs/AUDIO.md). Gated on the kernel because its "in
// context" rows ask the kernel for a plan and play that plan's cues.
export default function SoundsPage() {
  return (
    <KernelGate>
      <ErrorBoundary context="Sounds Page">
        <SoundAudition />
      </ErrorBoundary>
    </KernelGate>
  );
}
