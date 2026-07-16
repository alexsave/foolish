'use client';

import React from 'react';
import { ensureBotsAsync } from '../../supabase/functions/_shared/sdk/ts/wasm/bots';
import { AuthProvider } from '../contexts/AuthContext';
import { LocalizationProvider } from '../contexts/LocalizationContext';
import { ThemeProvider } from '../contexts/ThemeContext';
import { StyleProvider } from '../contexts/StyleContext';
import { TextureProvider } from '../components/TexturedSurface';
import { WoolBackgroundLayer } from '../components/WoolBackgroundLayer';
import { ErrorBoundary } from '../components/ErrorBoundary';

// The kernel gate (A8/F7).
//
// The browser reads the wire formats through the kernel now — decodePackedGame
// and decodeEventWire both call into bots.wasm — and the module has to be
// FETCHED (there is no filesystem), so bots() throws until it is warm. The web
// used to decode with pure TypeScript that shadowed view.c and evwire.c byte for
// byte; this fetch is what that mirror cost, and it is paid once per session
// against a cached asset.
//
// A gate, not a fire-and-forget warm, because the realtime subscription's
// applyRow is a synchronous callback: "probably warm by the time a board
// arrives" is the kind of nearly-always-true that fails on a cold cache and a
// fast server. Rendering nothing shows the app background through — see the
// WoolBackgroundLayer note below — so this reads as the page still loading, not
// as a blank screen.
function KernelGate({ children }: { children: React.ReactNode }) {
  const [ready, setReady] = React.useState(false);
  const [failed, setFailed] = React.useState<Error | null>(null);
  React.useEffect(() => {
    let cancelled = false;
    ensureBotsAsync()
      .then(() => { if (!cancelled) setReady(true); })
      .catch((e) => { if (!cancelled) setFailed(e instanceof Error ? e : new Error(String(e))); });
    return () => { cancelled = true; };
  }, []);
  // A kernel that will not load is not a degraded app, it is no app: every board
  // on every screen is decoded through it. Say so rather than hanging on a
  // background forever.
  if (failed) throw failed;
  return ready ? <>{children}</> : null;
}

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
                  {/* Single app-wide background behind every page. Any loading
                      or redirect state can simply render nothing and this shows
                      through, so there is never a bare white screen. */}
                  <div className="app-background">
                    <WoolBackgroundLayer />
                  </div>
                  <AuthProvider>
                    <ErrorBoundary context="Auth Provider">
                      <ErrorBoundary context="Routes">
                        <KernelGate>{children}</KernelGate>
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
