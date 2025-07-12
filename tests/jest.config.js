module.exports = {
  preset: 'ts-jest',
  testEnvironment: 'node',
  rootDir: '../',
  roots: ['<rootDir>'],
  testMatch: ['<rootDir>/tests/**/*.test.ts'],
  transform: {
    '^.+\\.ts$': 'ts-jest',
  },
  collectCoverageFrom: [
    'supabase/functions/_shared/actions/*.ts',
    'supabase/functions/_shared/utils.ts',
    'supabase/functions/_shared/common_utils.ts',
    '!**/*.d.ts',
  ],
  coverageDirectory: 'tests/coverage',
  coverageReporters: ['text', 'lcov', 'html'],
  coverageThreshold: {
    global: {
      branches: 80,
      functions: 80,
      lines: 80,
      statements: 80,
    },
  },
  setupFilesAfterEnv: ['<rootDir>/tests/setup.ts'],
  moduleFileExtensions: ['ts', 'js'],
  testTimeout: 10000,
  moduleNameMapper: {
    '^jsr:@supabase/supabase-js$': '<rootDir>/tests/mocks/supabase.ts',
    '^jsr:@supabase/functions-js/edge-runtime.d.ts$': '<rootDir>/tests/mocks/supabase.ts',
    '^https://deno.land/std@0.168.0/http/server.ts$': '<rootDir>/tests/mocks/supabase.ts',
    '^../../supabase/functions/_shared/utils$': '<rootDir>/tests/mocks/utils.ts',
    '^../supabase/functions/_shared/utils$': '<rootDir>/tests/mocks/utils.ts',
    '^../utils\\.ts$': '<rootDir>/tests/mocks/utils.ts',
    '^\\.\\./_shared/utils\\.ts$': '<rootDir>/tests/mocks/utils.ts',
    '^\\.\\./_shared/utils$': '<rootDir>/tests/mocks/utils.ts',
  },
  transformIgnorePatterns: [
    "node_modules/(?!(jsr|@supabase)/)"
  ],
}; 