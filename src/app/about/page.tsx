'use client';

import { ErrorBoundary } from '../../components/ErrorBoundary';
import { About } from '../../components/About';

export default function AboutPage() {
  return (
    <ErrorBoundary context="About Page">
      <About />
    </ErrorBoundary>
  );
}
