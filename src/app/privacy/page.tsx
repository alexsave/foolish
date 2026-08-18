'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { Privacy } from '../../components/Privacy';

export default function PrivacyPage() {
  return (
    <ErrorBoundary context="Privacy Page">
      <Privacy />
    </ErrorBoundary>
  );
}
