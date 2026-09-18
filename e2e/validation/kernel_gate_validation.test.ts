// The kernel gate belongs to the routes that run the kernel, and to no others.
//
// `KernelGate` renders NOTHING until `bots.wasm` has been fetched, and throws if
// the fetch fails. That is correct for a game surface - the browser runs kernel
// code synchronously on the render and realtime paths, and `ServerContext`'s
// `applyRow` is a synchronous callback that cannot wait - and it is wrong for a
// page whose whole job is to be readable.
//
// It is wrong in a way nothing else catches. Prerendering runs the server pass,
// where the gate's `useEffect` never fires, so a gated route's static HTML holds
// only the `<noscript>` line. In a browser it looks perfect. To `curl`, to a
// crawler, and to an App Store reviewer's link checker it is blank. That is how
// `foolish.cards/support` - a URL filed on a submission that was sitting in
// review - came to serve one sentence of text, and why `/privacy`, `/support`
// and `/imessage-privacy` are now static files in `public/` instead
// (`docs/IMESSAGE_APP_STORE_SUBMISSION.md` §8).
//
// So the gate cannot live in the root `Providers`, where it silently covers
// every route that will ever exist. It lives on the pages that need it, and this
// file is what keeps that list honest: the answer is DERIVED from each page's
// real import graph rather than written down, because a hand-maintained list of
// "routes that need a kernel" rots exactly like every other hand-maintained list
// in this repo.
//
// Read both directions of the assertion, they catch opposite mistakes:
//   - a page that reaches the kernel WITHOUT a gate is a crash (bots() throws
//     cold), and
//   - a page that does not reach the kernel WITH a gate is a blank page to
//     everything that does not run scripts.
//
// Pure test - no Postgres, no network, no build.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');

/** The module whose exports only work once the wasm is warm. */
const KERNEL = join(REPO, 'sdk/ts/wasm/bots.ts');

/** The gate itself. Importing it is never evidence that a page needs it. */
const GATE = join(REPO, 'src/components/KernelGate.tsx');

/** The root client boundary. The gate must NOT be here - that is the whole point. */
const PROVIDERS = join(REPO, 'src/app/providers.tsx');

/** tsconfig.json `compilerOptions.paths`, as the bundler resolves them. */
const ALIASES: Record<string, string> = {
    '@shared/': 'server/impls/supabase/functions/_shared/',
    '@api/': 'server/api/',
    '@sdk/': 'sdk/',
};

/** Every quoted specifier in `from '...'`, `import('...')` and `new URL('...')`. */
const SPEC = /(?:from|import|URL)\s*\(?\s*['"]([^'"]+)['"]/g;

function resolveSpec(spec: string, fromFile: string): string | null {
    let base: string | null = null;
    for (const [alias, target] of Object.entries(ALIASES)) {
        if (spec.startsWith(alias)) base = resolve(REPO, target, spec.slice(alias.length));
    }
    if (base === null && spec.startsWith('.')) base = resolve(dirname(fromFile), spec);
    if (base === null) return null;             // a bare package - not a file in this repo
    for (const cand of [base, `${base}.ts`, `${base}.tsx`, join(base, 'index.ts'), join(base, 'index.tsx')]) {
        if (existsSync(cand) && statSync(cand).isFile()) return cand;
    }
    return null;
}

/**
 * The transitive closure of what a route entry pulls in, with the gate module
 * itself cut out of the graph.
 *
 * Cutting it is not a convenience, it is what makes the question answerable: a
 * gated page imports `KernelGate`, which imports the kernel, so following that
 * edge would make every gated page "need a gate" because it has one.
 */
function graphOf(entry: string): Set<string> {
    const seen = new Set<string>();
    const queue = [entry];
    while (queue.length) {
        const file = queue.pop()!;
        if (seen.has(file) || file === GATE || !existsSync(file)) continue;
        seen.add(file);
        for (const m of readFileSync(file, 'utf8').matchAll(SPEC)) {
            const target = resolveSpec(m[1], file);
            if (target && !seen.has(target)) queue.push(target);
        }
    }
    return seen;
}

function walk(dir: string, out: string[] = []): string[] {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const p = join(dir, entry.name);
        if (entry.isDirectory()) walk(p, out); else out.push(p);
    }
    return out;
}

/**
 * The client entry of every route: `page.tsx` plus the `not-found.tsx` fallback.
 *
 * Deliberately NOT layouts. `src/app/m/[payload]/layout.tsx` runs the kernel in
 * `generateMetadata`, on the SERVER, so an unfurl carries a true description -
 * that is not a client render and no gate belongs anywhere near it.
 */
function routeEntries(): string[] {
    return walk(join(REPO, 'src/app'))
        .filter((p) => /\/(page|not-found)\.tsx$/.test(p))
        .sort();
}

/** Does this file render `<KernelGate>`? */
function gated(file: string): boolean {
    return /<KernelGate[\s>]/.test(readFileSync(file, 'utf8'));
}

const entries = routeEntries();

test('the route entries are discoverable at all', () => {
    // Every assertion below is vacuously true against an empty list, so the
    // routes moving house must break this test rather than quietly pass them.
    assert.ok(entries.length >= 8,
        `found only ${entries.length} route entries under src/app - did the layout move?`);
    assert.ok(existsSync(KERNEL), `the kernel module is not at ${relative(REPO, KERNEL)}`);
    assert.ok(existsSync(GATE), `the gate is not at ${relative(REPO, GATE)}`);
});

test('every route that reaches the kernel renders the gate', () => {
    const ungated = entries
        .filter((p) => graphOf(p).has(KERNEL) && !gated(p))
        .map((p) => relative(REPO, p));
    assert.deepEqual(ungated, [],
        'these routes run kernel code and would call bots() before the wasm is warm.\n'
        + 'Wrap the route in <KernelGate> (src/components/KernelGate.tsx):\n'
        + ungated.join('\n'));
});

test('no route that skips the kernel renders the gate', () => {
    const overGated = entries
        .filter((p) => !graphOf(p).has(KERNEL) && gated(p))
        .map((p) => relative(REPO, p));
    assert.deepEqual(overGated, [],
        'these routes need no kernel, and a gate makes them render NOTHING until a\n'
        + 'wasm fetch nobody is waiting for finishes - blank to curl, to crawlers and\n'
        + 'to a store reviewer. Drop the <KernelGate>:\n'
        + overGated.join('\n'));
});

test('the gate is not in the root providers, where it would cover every future route', () => {
    // The regression this file exists to prevent, stated as its own assertion:
    // one <KernelGate> here silently re-gates /about, /leaderboard and every page
    // added after it, and the two tests above would still pass.
    assert.ok(!gated(PROVIDERS),
        `${relative(REPO, PROVIDERS)} renders <KernelGate>. A gate in the root boundary `
        + 'covers every route that will ever exist, including the ones whose only job is '
        + 'to be readable without JavaScript.');
    assert.ok(!graphOf(PROVIDERS).has(KERNEL),
        `${relative(REPO, PROVIDERS)} still reaches the kernel, so the root layout does too `
        + 'and every route pays for the module it may never use.');
});

test('the split is real in both directions', () => {
    // A rule satisfied by "no route reaches the kernel" or "every route does" is
    // not a rule. Both sides have to be populated for the two tests above to mean
    // anything, and this says so out loud rather than leaving it to inspection.
    const kernelRoutes = entries.filter((p) => graphOf(p).has(KERNEL)).map((p) => relative(REPO, p));
    const plainRoutes = entries.filter((p) => !graphOf(p).has(KERNEL)).map((p) => relative(REPO, p));
    assert.ok(kernelRoutes.length > 0, 'no route reaches the kernel - the graph walk is broken');
    assert.ok(plainRoutes.length > 0,
        'every route reaches the kernel, so the gate is global again in all but name');
    // The pages an App Store record and a crawler actually read.
    for (const p of ['src/app/about/page.tsx']) {
        assert.ok(plainRoutes.includes(p),
            `${p} now reaches the kernel. It is the Marketing URL on a live store record `
            + 'and it must prerender its text without JavaScript.');
    }
});
