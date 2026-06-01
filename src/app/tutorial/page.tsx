'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { Tutorial } from '../../components/Tutorial';

export default function TutorialPage() {
  return (
    <ErrorBoundary context="Tutorial Page">
      <Tutorial />
    </ErrorBoundary>
  );
}
