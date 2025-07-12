// Mock for utils.ts - only handles JSR imports and database operations
// All pure utility functions are now in common_utils.ts and don't need mocking

import { mockCreateClient } from './supabase';

// Mock the JSR imports that cause issues in Jest
export const createClient = mockCreateClient;
export const wrap400 = jest.fn();
export const verify_player_in_game = jest.fn();
export const personalize_game = jest.fn();
export const loadCompleteGame = jest.fn();
export const saveCompleteGame = jest.fn();
export const getPlayerHand = jest.fn();
export const updatePlayerHand = jest.fn();
export const broadcastToGameUser = jest.fn();
export const broadcastToGameUsers = jest.fn();
export const start_game = jest.fn();
export const refill = jest.fn();
export const check_win = jest.fn();
export const updateEloRatings = jest.fn();
export const getOrCreateEloRating = jest.fn();
export const getBotEloRating = jest.fn();
export const executeWithGameLock = jest.fn((gameId: string, operation: () => Promise<any>) => operation());
export const acquireGameLock = jest.fn();
export const releaseGameLock = jest.fn();
export const createId = jest.fn();

// Mock other imports
export const scheduleBotActions = jest.fn();
export const getAuthenticatedUser = jest.fn();
export const serve = jest.fn();

// Mock Deno global
global.Deno = {
  env: {
    get: jest.fn()
  }
} as any; 