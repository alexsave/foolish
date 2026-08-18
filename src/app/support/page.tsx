'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { Support } from '../../components/Support';

export default function SupportPage() {
  return (
    <ErrorBoundary context="Support Page">
      <Support />
    </ErrorBoundary>
  );
}
