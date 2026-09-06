// The gate's own gate.
//
// scripts/check_determinism.mjs is the thing standing between this repo and a
// second round of "Math.random crept back into the game path". A gate nobody
// has watched go red is not a gate, and this repo has shipped several of those:
// a check that passes for the wrong reason looks exactly like a check that
// passes.
//
// So this points scan() at fixture trees written here, one per rule, and asserts
// it goes red on each - and green on the shapes it must NOT flag, which is the
// half that decides whether anyone can live with it. A gate that fires on the
// word "Math.random" inside a comment explaining why Math.random was removed
// gets deleted by the third person who trips over it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';

// @ts-expect-error - plain .mjs, no types, deliberately runnable by bare node.
import { scan, RULES, ALLOW } from '../scripts/check_determinism.mjs';

type Files = Record<string, string>;

/** Write a fixture tree and scan it with no allowlist unless one is given. */
function scanFixture(files: Files, allow: unknown[] = []): string[] {
    const root = mkdtempSync(join(tmpdir(), 'detgate-'));
    try {
        for (const [rel, src] of Object.entries(files)) {
            const abs = join(root, rel);
            mkdirSync(dirname(abs), { recursive: true });
            writeFileSync(abs, src);
        }
        return scan({ root, allow });
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
}

const red = (files: Files, allow: unknown[] = []) => scanFixture(files, allow);

test('the gate goes red on every kind of draw it claims to catch', () => {
    const cases: [string, Files][] = [
        ['Math.random in a suite', { 'e2e/x.test.ts': 'const a = Math.random();' }],
        ['Math.random in the sdk', { 'sdk/ts/x.ts': 'export const a = () => Math.random();' }],
        ['Math.random in the server', { 'server/api/x.ts': 'const a = Math.floor(Math.random() * 3);' }],
        ['crypto.randomUUID', { 'sdk/ts/x.ts': 'const id = crypto.randomUUID();' }],
        ['a bare randomUUID import', { 'e2e/h.ts': "import {randomUUID} from 'crypto';\nconst id = randomUUID();" }],
        ['a deep receiver chain', { 'server/api/x.ts': 'const id = globalThis.crypto.randomUUID();' }],
        ['crypto.getRandomValues', { 'sdk/ts/x.ts': 'crypto.getRandomValues(new Uint8Array(4));' }],
        ['randomBytes', { 'server/api/x.ts': 'const b = randomBytes(16);' }],
        ['Date.now deciding a test', { 'e2e/x.test.ts': 'assert.ok(Date.now() - t0 < 5);' }],
        ['performance.now in a test', { 'e2e/x.test.ts': 'const t = performance.now();' }],
        ['a no-arg new Date in a test', { 'e2e/x.test.ts': 'const d = new Date();' }],
    ];
    for (const [what, files] of cases) {
        assert.ok(red(files).length > 0, `the gate must fail on: ${what}`);
    }
});

test('the gate stays green on the shapes that only look like draws', () => {
    const cases: [string, Files][] = [
        // The comments in engine.ts and bots.ts that record the REMOVED reseeds.
        ['a mention in a line comment', { 'sdk/ts/x.ts': '// replaces the old per-decision Math.random\nexport const a = 1;' }],
        ['a mention in a block comment', { 'sdk/ts/x.ts': '/* was crypto.randomUUID() once */\nexport const a = 1;' }],
        ['a mention in a string', { 'e2e/x.test.ts': "const msg = 'no Math.random() here';" }],
        ['a mention in a template literal', { 'e2e/x.test.ts': 'const msg = `use Date.now() never`;' }],
        // bot_parity.test.ts pins code it does not own by ASSIGNING the global.
        ['patching the global, not drawing', { 'e2e/x.test.ts': 'Math.random = mkLcg(seed);' }],
        ['restoring the global', { 'e2e/x.test.ts': 'Math.random = realRandom;' }],
        // A conversion, not a reading.
        ['new Date with an argument', { 'e2e/x.test.ts': 'const d = new Date(FIXED_TS).toISOString();' }],
        ['new Date(ms) from a variable', { 'e2e/x.test.ts': 'const d = new Date(t * 1000);' }],
        // A different function that merely ends in the same letters.
        ['a same-suffix identifier', { 'e2e/x.test.ts': 'const id = myRandomUUID();' }],
        // The clock rule is scoped to test FILES; a bench measuring itself is fine.
        ['a clock read in a bench, not a test', { 'e2e/bench.ts': 'const t0 = Date.now();' }],
        ['a clock read in a harness', { 'e2e/adapters/x.ts': 'const at = Date.now();' }],
        // The clock rule does not reach the server, which must read a real clock.
        ['a clock read on the server', { 'server/api/x.ts': 'const now = Date.now();' }],
    ];
    for (const [what, files] of cases) {
        assert.deepEqual(red(files), [], `the gate must NOT fail on: ${what}`);
    }
});

test('an allowlist entry is per rule and per file, and must be spent', () => {
    const files: Files = { 'server/api/x.ts': 'const a = Math.random();' };
    const entry = { rule: 'math-random', file: 'server/api/x.ts', calls: 1, reason: 'because' };

    assert.deepEqual(red(files, [entry]), [], 'an allowlisted call passes');

    // A SECOND draw in an allowed file still fails - the count is the teeth.
    assert.ok(
        red({ 'server/api/x.ts': 'const a = Math.random(); const b = Math.random();' }, [entry]).length > 0,
        'a second draw in an allowlisted file must still fail',
    );

    // The entry does not cover a different rule in the same file.
    assert.ok(
        red({ 'server/api/x.ts': 'const a = Math.random(); const i = crypto.randomUUID();' }, [entry]).length > 0,
        'an allowlist entry must not cover a different rule',
    );

    // An entry with nothing left to allow fails, so the list cannot rot.
    assert.ok(
        red({ 'server/api/x.ts': 'const a = 1;' }, [entry]).length > 0,
        'a stale allowlist entry must fail',
    );

    // A typo in the rule name must not silently allow anything.
    assert.ok(
        red(files, [{ ...entry, rule: 'math-randmo' }]).length > 0,
        'an allowlist entry naming an unknown rule must fail',
    );
});

test('the shipped allowlist is legible: every entry names a live rule and says why', () => {
    const ids = new Set(RULES.map((r: { id: string }) => r.id));
    for (const a of ALLOW as { rule: string; file: string; calls: number; reason: string }[]) {
        assert.ok(ids.has(a.rule), `allowlist entry for ${a.file} names unknown rule ${a.rule}`);
        assert.ok(a.calls > 0, `allowlist entry for ${a.file} allows zero calls`);
        // The reason is the whole point of the entry - a bare "ok" is not one.
        assert.ok(a.reason.length > 60, `allowlist entry for ${a.file} needs a real reason, not "${a.reason}"`);
    }
});
