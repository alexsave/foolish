// The deploy trigger must list every path the deployed server is BUILT from.
//
// A path missing from `.github/workflows/deploy.yml`'s `on.push.paths` fails
// nothing and says nothing: the PR merges green, no deploy runs, and production
// keeps serving the old code until some unrelated push happens to touch a path
// that IS listed. It has happened once already - after A10 the trigger still
// watched the repo-root `supabase/**`, which had stopped existing, and the
// server went stale for weeks with every check green.
//
// The list cannot be maintained by memory, because the dependency is invisible
// from the workflow: the edge functions import through the `@sdk/` and `@api/`
// aliases in `functions/import_map.json`, so `sdk/ts/wire/view.ts` is compiled
// into the deployed server while looking, from the workflow's side, like
// unrelated client code.
//
// So this walks the functions' REAL import graph - every entrypoint, every
// static import and re-export, transitively, aliases resolved the way Deno
// resolves them - plus the static assets named in `config.toml`, and asserts
// each one is covered. It also asserts a NEW file dropped next to an existing
// dependency would be covered, so a trigger narrowed to today's exact files
// cannot pass.
//
// Pure test - no Postgres, no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const SUPA = join(REPO, 'server/impls/supabase');
const FUNCTIONS = join(SUPA, 'functions');
const WORKFLOW = join(REPO, '.github/workflows/deploy.yml');

/** `@sdk/` -> `../../../../sdk/`, exactly as the deployed functions resolve it. */
const aliases: Record<string, string> =
    JSON.parse(readFileSync(join(FUNCTIONS, 'import_map.json'), 'utf8')).imports;

/** Every quoted specifier in `from '...'`, `import('...')` and `new URL('...')`. */
const SPEC = /(?:from|import|URL)\s*\(?\s*['"]([^'"]+)['"]/g;

function resolveSpec(spec: string, fromFile: string): string | null {
    for (const [alias, target] of Object.entries(aliases)) {
        if (spec.startsWith(alias)) return resolve(FUNCTIONS, target, spec.slice(alias.length));
    }
    if (spec.startsWith('.')) return resolve(dirname(fromFile), spec);
    return null;   // jsr:/https:/npm: - not a file in this repo
}

/** The transitive closure of what `supabase functions deploy` compiles. */
function compiledFiles(): string[] {
    const entrypoints = readdirSync(FUNCTIONS)
        .map((name) => join(FUNCTIONS, name, 'index.ts'))
        .filter((p) => existsSync(p) && statSync(p).isFile());
    assert.ok(entrypoints.length > 0, 'found no edge-function entrypoints - did the layout move?');

    const seen = new Set<string>();
    const queue = [...entrypoints];
    while (queue.length) {
        const f = queue.pop()!;
        if (seen.has(f) || !existsSync(f)) continue;
        seen.add(f);
        for (const m of readFileSync(f, 'utf8').matchAll(SPEC)) {
            const target = resolveSpec(m[1], f);
            if (target && existsSync(target) && !seen.has(target)) queue.push(target);
        }
    }
    return [...seen].map((p) => relative(REPO, p));
}

/** The assets bundled alongside the functions, from config.toml's static_files. */
function staticFiles(): string[] {
    const toml = readFileSync(join(SUPA, 'config.toml'), 'utf8');
    const out: string[] = [];
    for (const line of toml.split('\n')) {
        const trimmed = line.trim();
        if (trimmed.startsWith('#') || !trimmed.startsWith('static_files')) continue;
        for (const m of trimmed.matchAll(/"([^"]+)"/g)) {
            out.push(relative(REPO, resolve(SUPA, m[1])));
        }
    }
    return [...new Set(out)];   // every function bundles the same wasm blob
}

/** `on.push.paths` from the workflow, read as the literal list of globs. */
function triggerPaths(): string[] {
    const lines = readFileSync(WORKFLOW, 'utf8').split('\n');
    const start = lines.findIndex((l) => /^\s{4}paths:\s*$/.test(l));
    assert.ok(start >= 0, 'deploy.yml has no on.push.paths list');
    const out: string[] = [];
    for (const line of lines.slice(start + 1)) {
        const m = line.match(/^\s+-\s+'([^']+)'\s*$/);
        if (m) { out.push(m[1]); continue; }
        if (/^\s*#/.test(line) || line.trim() === '') continue;
        break;                                  // the list ended
    }
    assert.ok(out.length > 0, 'deploy.yml lists no trigger paths at all');
    return out;
}

/** GitHub's path filter, for the two wildcards this list uses. */
function matches(glob: string, path: string): boolean {
    const rx = glob
        .split('**').map((part) => part.split('*')
            .map((lit) => lit.replace(/[.+?^${}()|[\]\\]/g, '\\$&'))
            .join('[^/]*'))
        .join('.*');
    return new RegExp(`^${rx}$`).test(path);
}

const globs = triggerPaths();
const covered = (p: string) => globs.some((g) => matches(g, p));

test('every file compiled into the deployed edge functions triggers a deploy', () => {
    const uncovered = compiledFiles().filter((p) => !covered(p));
    assert.deepEqual(uncovered, [],
        'these files are deployed but a change to them would NOT trigger deploy.yml:\n'
        + `${uncovered.join('\n')}\n`
        + `trigger paths: ${globs.join(', ')}`);
});

test('every static asset bundled with the functions triggers a deploy', () => {
    const assets = staticFiles();
    assert.ok(assets.length > 0, 'config.toml declares no static_files - did bots.wasm.gz stop shipping?');
    const uncovered = assets.filter((p) => !covered(p));
    assert.deepEqual(uncovered, [], `bundled but not watched: ${uncovered.join(', ')}`);
});

test('a NEW file beside a deployed one would trigger a deploy too', () => {
    // A trigger listing today's exact filenames would satisfy the test above and
    // still miss tomorrow's module. Every directory the graph reaches has to be
    // covered as a directory.
    const dirs = new Set(compiledFiles().map((p) => dirname(p)));
    const blind = [...dirs].filter((d) => !covered(`${d}/a_new_module.ts`)).sort();
    assert.deepEqual(blind, [],
        `a new file in these directories would deploy nothing: ${blind.join(', ')}`);
});
