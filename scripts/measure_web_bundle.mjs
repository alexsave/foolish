#!/usr/bin/env node
// The web bundle gate (docs/C_GAME_SHAPE_MIGRATION.md 4.0, "web bundle gz").
//
// What a browser downloads as JavaScript before the first paint of the two routes
// that matter: the landing page `/` and the table `/[game_id]`. That is the
// runtime chunks every route loads (build-manifest.json rootMainFiles) plus the
// route's own entry chunks, which include the root layout (the page's
// client-reference manifest, entryJSFiles). Lazily imported chunks, the legacy
// nomodule polyfill and bots.wasm.gz (a separately fetched asset, measured by
// collect_metrics.mjs) are not counted.
//
// Each file is gzipped at level 9 in memory and the sizes summed; a chunk shared
// by both routes counts once in the total.
//
//   node scripts/measure_web_bundle.mjs            # table on stdout
//   node scripts/measure_web_bundle.mjs --json     # one JSON object on stdout
//   node scripts/measure_web_bundle.mjs --in-place # build in this checkout's .next
//
// WHERE IT BUILDS. By default the tracked and untracked-but-not-ignored files are
// copied to a temporary directory and built there, so a measurement never
// rewrites the .next a `next dev` in the same checkout is serving, and several
// agents can share one worktree. node_modules is cloned (APFS copy-on-write on
// macOS), not symlinked: Turbopack refuses a node_modules symlink that points
// outside the project root, which is exactly what a git worktree has.
// --in-place (the default when CI is set) builds in the checkout itself.
//
// The Supabase URL and key are inlined into the client bundle, so they are pinned
// to fixed placeholders here: a measurement must not move because a machine has a
// different project URL in its environment.
//
// Exits non-zero when the build fails or a route is missing from the manifests.

import { execFileSync, spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import vm from 'node:vm';

// Each `manifest` is a path inside the .next build tree, not a path in this
// repo, so the marker below tells e2e/validation/helper_scripts_validation.test.ts
// not to look for it here.
export const ROUTES = [
    { route: '/', entry: '[project]/src/app/page', manifest: 'server/app/page_client-reference-manifest.js' },   // no-repo-path
    { route: '/[game_id]', entry: '[project]/src/app/[game_id]/page', manifest: 'server/app/[game_id]/page_client-reference-manifest.js' },   // no-repo-path
];

// Large trees the web build never reads. Skipping them keeps the staged copy
// small; anything else missing from the copy fails the build loudly.
const STAGE_SKIP = ['ios/', 'docs/'];

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const BUILD_ENV = {
    NEXT_PUBLIC_SUPABASE_URL: 'https://measure.supabase.co',
    NEXT_PUBLIC_SUPABASE_KEY: 'measure-web-bundle',
    NEXT_TELEMETRY_DISABLED: '1',
};

function stage(root) {
    const dir = mkdtempSync(join(tmpdir(), 'foolish-web-bundle-'));
    const listed = execFileSync('git', ['-C', root, 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
        { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 }).split('\0').filter(Boolean);
    for (const rel of listed) {
        if (rel === 'node_modules' || STAGE_SKIP.some((p) => rel.startsWith(p))) continue;
        const src = join(root, rel);
        if (!existsSync(src)) continue; // deleted in the working tree, not yet committed
        const dst = join(dir, rel);
        mkdirSync(dirname(dst), { recursive: true });
        cpSync(src, dst, { dereference: false });
    }
    const modules = realpathSync(join(root, 'node_modules'));
    const cp = process.platform === 'darwin'
        ? spawnSync('cp', ['-cR', modules, join(dir, 'node_modules')], { stdio: 'inherit' })
        : spawnSync('cp', ['-R', '--reflink=auto', modules, join(dir, 'node_modules')], { stdio: 'inherit' });
    if (cp.status !== 0) throw new Error(`could not copy node_modules from ${modules}`);
    return dir;
}

function build(dir) {
    const next = join(dir, 'node_modules', '.bin', 'next');
    const r = spawnSync(next, ['build'], {
        cwd: dir, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024,
        env: { ...process.env, ...BUILD_ENV },
    });
    if (r.status !== 0) {
        const log = `${r.stdout ?? ''}${r.stderr ?? ''}`;
        throw new Error(`next build failed (exit ${r.status})\n${log.split('\n').slice(-40).join('\n')}`);
    }
}

// The client-reference manifest is a script that assigns into
// globalThis.__RSC_MANIFEST; run it in a sandbox rather than parsing it.
function entryFiles(distDir, { route, entry, manifest }) {
    const path = join(distDir, manifest);
    if (!existsSync(path)) throw new Error(`no client-reference manifest for ${route} at ${path}`);
    const sandbox = { __RSC_MANIFEST: {} };
    sandbox.self = sandbox;
    sandbox.globalThis = sandbox;
    vm.runInNewContext(readFileSync(path, 'utf8'), sandbox);
    for (const page of Object.values(sandbox.__RSC_MANIFEST)) {
        const files = page?.entryJSFiles?.[entry];
        if (files) return files;
    }
    throw new Error(`${route}: ${manifest} lists no entryJSFiles for ${entry}`);
}

function sizeOf(distDir, files) {
    let raw = 0, gz = 0;
    for (const f of files) {
        const bytes = readFileSync(join(distDir, f));
        raw += bytes.length;
        gz += gzipSync(bytes, { level: 9 }).length;
    }
    return { files: files.length, raw, gz };
}

/** Build and measure. Returns { routes: {route: {files, raw, gz}}, total: {files, raw, gz} }. */
export function measureWebBundle({ inPlace = Boolean(process.env.CI), keep = false } = {}) {
    const dir = inPlace ? REPO : stage(REPO);
    try {
        build(dir);
        const distDir = join(dir, '.next');
        const buildManifest = JSON.parse(readFileSync(join(distDir, 'build-manifest.json'), 'utf8'));
        const runtime = buildManifest.rootMainFiles ?? [];
        if (runtime.length === 0) throw new Error('build-manifest.json lists no rootMainFiles');
        const routes = {};
        const all = new Set();
        for (const r of ROUTES) {
            const files = [...new Set([...runtime, ...entryFiles(distDir, r)])].sort();
            files.forEach((f) => all.add(f));
            routes[r.route] = sizeOf(distDir, files);
        }
        return { routes, total: sizeOf(distDir, [...all].sort()) };
    } finally {
        if (!inPlace && !keep) rmSync(dir, { recursive: true, force: true });
    }
}

function table(m) {
    const rows = [...Object.entries(m.routes), ['total (union)', m.total]];
    const w = Math.max(...rows.map(([k]) => k.length));
    const lines = [`${'route'.padEnd(w)}  files  ${'raw B'.padStart(9)}  ${'gzip B'.padStart(9)}`];
    for (const [k, v] of rows) {
        lines.push(`${k.padEnd(w)}  ${String(v.files).padStart(5)}  ${String(v.raw).padStart(9)}  ${String(v.gz).padStart(9)}`);
    }
    return lines.join('\n');
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const args = new Set(process.argv.slice(2));
    try {
        const m = measureWebBundle({
            inPlace: args.has('--in-place') || (Boolean(process.env.CI) && !args.has('--staged')),
            keep: args.has('--keep'),
        });
        process.stdout.write(args.has('--json') ? `${JSON.stringify(m)}\n` : `${table(m)}\n`);
    } catch (e) {
        process.stderr.write(`measure_web_bundle: ${e.message}\n`);
        process.exit(1);
    }
}
