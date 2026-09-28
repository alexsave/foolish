// Ambient declarations for type-checking the e2e suites (e2e/tsconfig.json).
//
// Only what the Node test process really has at run time and the pinned type
// packages do not describe.

// The edge functions read their environment through Deno.env. Under Node the
// harness installs this much of Deno before any server module loads
// (e2e/harness.ts), so this is the shape the tests run the server code against.
declare namespace Deno {
    const env: { get(key: string): string | undefined };
}

// jsdom ships no types and @types/jsdom is not a dependency; the suites that
// render React into it use the constructor and its window.
declare module 'jsdom' {
    export class JSDOM {
        constructor(html?: string, options?: Record<string, unknown>);
        readonly window: Window & typeof globalThis;
        serialize(): string;
    }
}

// Node 22 added fs.globSync; @types/node is pinned at 20 while the suites run
// on a newer Node.
declare module 'node:fs' {
    export function globSync(pattern: string | readonly string[], options?: { cwd?: string; exclude?: (path: string) => boolean }): string[];
}
