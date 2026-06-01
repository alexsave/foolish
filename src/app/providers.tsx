'use client';

import React from 'react';
import { AuthProvider } from '../contexts/AuthContext';
import { LocalizationProvider } from '../contexts/LocalizationContext';
import { ThemeProvider } from '../contexts/ThemeContext';
import { StyleProvider } from '../contexts/StyleContext';
import { TextureProvider } from '../components/TexturedSurface';
import { ErrorBoundary } from '../components/ErrorBoundary';

// Global providers that previously wrapped every route in App.js. With the
// Next.js App Router these live in the root layout so they persist across
// client-side navigations. Imported statically (not via dynamic/ssr:false) so
// they are bundled and preloaded with the page rather than fetched as a
// separate late-discovered chunk — this avoids a client-side load waterfall.
export default function Providers({ children }: { children: React.ReactNode }) {
  return (
    <ErrorBoundary context="App Root">
      <div style={{ display: 'flex', height: '100vh', width: '100vw' }}>
        <ErrorBoundary context="Router">
          <LocalizationProvider>
            <ThemeProvider>
              <StyleProvider>
                <TextureProvider>
                  <AuthProvider>
                    <ErrorBoundary context="Auth Provider">
                      <ErrorBoundary context="Routes">
                        {children}
                      </ErrorBoundary>
                    </ErrorBoundary>
                  </AuthProvider>
                </TextureProvider>
              </StyleProvider>
            </ThemeProvider>
          </LocalizationProvider>
        </ErrorBoundary>
      </div>
    </ErrorBoundary>
  );
}
