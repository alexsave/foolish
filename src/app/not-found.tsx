'use client';

import { useEffect } from 'react';
import { useRouter } from 'next/navigation';

// Mirrors the old react-router catch-all (`path="*"` -> Navigate to /dashboard).
//
// This redirects from a client effect rather than throwing `redirect()` during
// the server render. A server NotFound that aborts mid-render leaves React's
// RSC performance tracker (flushComponentPerformance) with an end-time before
// its start-time, and `performance.measure('NotFound', …)` then throws
// "cannot have a negative time stamp" on every route in dev. Completing the
// render normally (returning null, redirecting from an effect) avoids that.
export default function NotFound() {
  const router = useRouter();
  useEffect(() => {
    router.replace('/dashboard');
  }, [router]);
  return null;
}
