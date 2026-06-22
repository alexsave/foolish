import { createClient } from '@supabase/supabase-js';
// Type-only: erased at build, never pulls supabase-js into the runtime graph.
import type { SupabaseClient } from '@supabase/supabase-js';

class Connector {
  private static instance: Connector;
  private client: SupabaseClient | undefined;
  constructor() {
    if (Connector.instance) {
      return Connector.instance;
    }

    this.client = createClient(
      process.env.NEXT_PUBLIC_SUPABASE_URL,
      process.env.NEXT_PUBLIC_SUPABASE_KEY
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
