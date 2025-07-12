// Mock Supabase client
export const mockSupabaseClient = {
  from: jest.fn(() => ({
    select: jest.fn().mockReturnThis(),
    update: jest.fn().mockReturnThis(),
    upsert: jest.fn().mockReturnThis(),
    insert: jest.fn().mockReturnThis(),
    delete: jest.fn().mockReturnThis(),
    eq: jest.fn().mockReturnThis(),
    single: jest.fn().mockResolvedValue({ data: null, error: null }),
  })),
  rpc: jest.fn(),
  channel: jest.fn(() => ({
    send: jest.fn(),
  })),
  removeChannel: jest.fn(),
};

// Mock createClient function
export const mockCreateClient = jest.fn().mockReturnValue(mockSupabaseClient);

// Export for JSR imports
export const createClient = mockCreateClient;

// Mock User type
export const mockUser = {
  id: 'test-user-id',
  email: 'test@example.com',
  user_metadata: {
    username: 'testuser',
  },
};

// Export User type for JSR imports
export const User = mockUser;

// Mock auth functions
export const mockGetAuthenticatedUser = jest.fn().mockResolvedValue(mockUser);

// Mock database operations
export const mockDatabaseOperations = {
  loadCompleteGame: jest.fn(),
  saveCompleteGame: jest.fn(),
  updatePlayerHand: jest.fn(),
  broadcastToGameUsers: jest.fn(),
  broadcastToGameUser: jest.fn(),
  getOrCreateEloRating: jest.fn(),
  getBotEloRating: jest.fn(),
  updateEloRatings: jest.fn(),
  acquireGameLock: jest.fn().mockResolvedValue(true),
  releaseGameLock: jest.fn().mockResolvedValue(undefined),
  executeWithGameLock: jest.fn().mockImplementation(async (game_id: string, operation: () => Promise<any>) => {
    return await operation();
  }),
};

// Mock bot actions
export const mockScheduleBotActions = jest.fn();

// Mock Deno HTTP server
export const serve = jest.fn(); 