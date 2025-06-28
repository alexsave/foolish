import { createClient } from '@supabase/supabase-js';
import { SupabaseClient } from '@supabase/supabase-js';

class Connector {
  private static instance: Connector;
  private client: SupabaseClient | undefined;
  constructor() {
    if (Connector.instance) {
      return Connector.instance;
    }

    this.client = createClient(
      process.env.REACT_APP_SUPABASE_URL,
      process.env.REACT_APP_SUPABASE_KEY
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