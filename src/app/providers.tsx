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
// client-side navigations. Loaded client-only (see AppShell) because the
// providers read localStorage/window during render, matching the old CRA
// behavior of rendering entirely in the browser.
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
