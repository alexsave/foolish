// Mock global Deno object
(global as any).Deno = {
  env: {
    get: jest.fn().mockReturnValue('mock-value'),
  },
  serve: jest.fn(),
};

// Mock crypto object
(global as any).crypto = {
  randomUUID: jest.fn().mockReturnValue('123e4567-e89b-12d3-a456-426614174000'),
};

// Mock setTimeout/clearTimeout
(global as any).setTimeout = jest.fn().mockImplementation((callback) => {
  return 1; // Mock timer ID
});

(global as any).clearTimeout = jest.fn();

// Mock console methods to avoid noise in tests
jest.spyOn(console, 'log').mockImplementation(() => {});
jest.spyOn(console, 'error').mockImplementation(() => {});
jest.spyOn(console, 'warn').mockImplementation(() => {});

// Mock Supabase modules that are imported via JSR
jest.mock('jsr:@supabase/supabase-js', () => ({
  createClient: jest.fn(() => ({
    rpc: jest.fn(),
    from: jest.fn(() => ({
      select: jest.fn().mockReturnThis(),
      update: jest.fn().mockReturnThis(),
      upsert: jest.fn().mockReturnThis(),
      insert: jest.fn().mockReturnThis(),
      delete: jest.fn().mockReturnThis(),
      eq: jest.fn().mockReturnThis(),
      single: jest.fn().mockResolvedValue({ data: null, error: null }),
    })),
    channel: jest.fn(() => ({
      send: jest.fn(),
    })),
    removeChannel: jest.fn(),
  })),
}));

// Mock Supabase Edge Functions runtime
jest.mock('jsr:@supabase/functions-js/edge-runtime.d.ts', () => ({}));

// Mock Deno HTTP server
jest.mock('https://deno.land/std@0.168.0/http/server.ts', () => ({
  serve: jest.fn(),
}));

// Mock auth.ts to avoid import issues
jest.mock('../supabase/functions/_shared/auth.ts', () => ({
  getAuthenticatedUser: jest.fn().mockResolvedValue({
    id: 'test-user-id',
    email: 'test@example.com',
    user_metadata: {
      username: 'testuser',
    },
  }),
}));

// Mock bot_actions.ts to avoid import issues
jest.mock('../supabase/functions/_shared/bot_actions.ts', () => ({
  scheduleBotActions: jest.fn(),
})); 