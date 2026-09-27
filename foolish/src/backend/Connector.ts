import { createClient } from '@supabase/supabase-js';
// Type-only: erased at build, never pulls supabase-js into the runtime graph.
import type { SupabaseClient } from '@supabase/supabase-js';

// auth-js serializes every auth call (getSession included) behind a cross-tab
// Web Lock and, for most calls, waits on it forever. Safari suspends background
// tabs WITHOUT releasing their Web Locks, so one frozen sibling tab blanks
// every new tab: the route guards render null until getSession resolves, and
// it never does. Bound the wait and fall back to running lock-less — a rare
// cross-tab token-refresh race is strictly better than an indefinite blank UI.
const LOCK_ACQUIRE_BUDGET_MS = 3000;
async function boundedNavigatorLock<R>(
  name: string,
  acquireTimeout: number,
  fn: () => Promise<R>
): Promise<R> {
  if (typeof navigator === 'undefined' || !navigator.locks) return fn();
  // auth-js passes -1 for "wait forever"; that is the case we must bound.
  const budget = acquireTimeout >= 0 ? acquireTimeout : LOCK_ACQUIRE_BUDGET_MS;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), budget);
  try {
    return await navigator.locks.request(
      name,
      { mode: 'exclusive', signal: controller.signal },
      async () => {
        clearTimeout(timer);
        return await fn();
      }
    );
  } catch (e) {
    if (e instanceof DOMException && e.name === 'AbortError') {
      return await fn();
    }
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

class Connector {
  private static instance: Connector;
  private client: SupabaseClient | undefined;
  constructor() {
    if (Connector.instance) {
      return Connector.instance;
    }

    this.client = createClient(
      process.env.NEXT_PUBLIC_SUPABASE_URL,
      process.env.NEXT_PUBLIC_SUPABASE_KEY,
      { auth: { lock: boundedNavigatorLock } }
    );

    Connector.instance = this;
  }

  getClient() {
    return this.client;
  }
}

// Create and export a singleton instance
const connector = new Connector();
export default connector.getClient()!;
