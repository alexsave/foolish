'use client';

import React from 'react';
import dynamic from 'next/dynamic';

// Render the whole provider tree (and therefore every page) client-only.
// The app is a client-rendered SPA that reads localStorage/window during
// render, so server-side rendering is disabled here — equivalent to how CRA
// shipped an empty shell and hydrated in the browser.
const Providers = dynamic(() => import('./providers'), { ssr: false });

export default function AppShell({ children }: { children: React.ReactNode }) {
  return <Providers>{children}</Providers>;
}
