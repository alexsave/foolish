'use client';

import React from 'react';
import { ensureBotsAsync } from '../../sdk/ts/wasm/bots';

// The kernel gate (A8/F7).
//
// The browser runs kernel code synchronously on the render and realtime paths
// (every envelope and push the page reads, through the client slot in
// sdk/ts/table/client_table.ts; the move guards, the optimistic-conflict and
// stale-animation rules, replay frames) and the module has to be FETCHED (there
// is no filesystem), so bots() throws until it is warm. The fetch is paid once
// per session against a cached asset.
//
// A gate, not a fire-and-forget warm, because the realtime subscription's
// applyRow is a synchronous callback: "probably warm by the time a board
// arrives" is the kind of nearly-always-true that fails on a cold cache and a
// fast server. Rendering nothing shows the app background through - see the
// WoolBackgroundLayer note in app/providers.tsx - so this reads as the page
// still loading, not as a blank screen.
//
// IT IS PER-ROUTE, NOT GLOBAL, and that is load-bearing. It used to wrap
// `children` in the root Providers, which meant it wrapped every route that
// would ever exist. Rendering nothing is the right answer for a game surface
// and the wrong one for a page whose job is to be readable: prerendering runs
// the server pass, where the effect below never fires, so a gated route's
// static HTML holds only the <noscript> line. In a browser it looks perfect; to
// curl, to a crawler and to a store reviewer's link checker it is blank. That
// is how foolish.cards/support - a URL filed on a submission then in review -
// came to serve one sentence of text.
//
// So it belongs to the routes that run the kernel: /, /[game_id], /dashboard,
// /history, /tutorial. The list is not maintained by hand. It is derived from
// each page's real import graph and asserted in BOTH directions by
// e2e/validation/kernel_gate_validation.test.ts - a page that reaches the
// kernel without a gate is a cold-start crash, and a page that does not reach
// it with a gate is a page nothing without JavaScript can read. Adding a route
// does not require remembering this comment; forgetting turns that test red.
export function KernelGate({ children }: { children: React.ReactNode }) {
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
  // on every screen behind this gate is decoded through it. Say so rather than
  // hanging on a background forever. The throw is caught by the "Routes"
  // ErrorBoundary in app/providers.tsx, the same boundary that caught it when
  // the gate lived there.
  if (failed) throw failed;
  return ready ? <>{children}</> : null;
}
