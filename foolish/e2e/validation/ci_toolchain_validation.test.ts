// Every CI job that can build the table host must install the toolchain first.
//
// c/build/table_host (c/tests/table_host.c, the native kernel the Node suites
// get a server's half of a game from) is never committed, so
// e2e/helpers/table_host.ts builds it on demand the first time a suite starts
// one. That build runs tools/structgen for the layout hash (c/Makefile, "Layout
// hash"), and structgen is a libclang program whose Makefile finds LLVM with
//
//     LLVM_PREFIX ?= $(shell llvm-config --prefix 2>/dev/null || echo /opt/homebrew/opt/llvm)
//
// so a Linux runner WITHOUT llvm-config silently falls back to the Mac's
// Homebrew path and the build dies on `clang-c/Index.h: No such file or
// directory`. The repo's answer is shared/scripts/ci_llvm.sh: it installs the
// toolchain and exports WASM_CC and LLVM_PREFIX for the rest of the job.
//
// This gate was written for the whole-kernel wasm test module the host
// replaced, whose lanes were fitted one at a time: validate.yml was missed, so
// `make` ran on a runner with no libclang and 26 validation scenarios went red
// at once - every one of them reporting a test failure whose real cause was a
// missing apt package, and nothing in the repo objected until CI did.
//
// A hand-written list of "lanes that need the toolchain" would rot the same
// way, so this derives the set: it walks the real import graph to find which
// test files reach the helper that shells out to `make`, maps npm scripts to
// the files they run, and maps workflow jobs to the scripts they run. Any job
// that can reach the build has to install the toolchain, and do it BEFORE the
// suite - a step that comes afterwards installs the compiler for a build that
// already failed.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
// Workflows live at the repository root, one level above the product.
const WORKFLOWS = join(resolve(REPO, '..'), '.github/workflows');

/** The helper that shells out to `make -C c table-host`. */
const BUILDER = join(REPO, 'e2e/helpers/table_host.ts');

/** Every quoted specifier in `from '...'` / `import('...')`, as deploy_paths reads them. */
const SPEC = /(?:from|import)\s*\(?\s*['"]([^'"]+)['"]/g;

/** Resolve a relative specifier to a file in this repo, or null. */
function resolveSpec(spec: string, fromFile: string): string | null {
    if (!spec.startsWith('.')) return null;      // node:, npm:, bare - not ours
    const base = resolve(dirname(fromFile), spec);
    for (const p of [base, `${base}.ts`, join(base, 'index.ts')]) {
        if (existsSync(p)) return p;
    }
    return null;
}

/** Does this file reach BUILDER, transitively? Memoised over the whole walk. */
const reaches = new Map<string, boolean>();
function buildsWasm(file: string, stack = new Set<string>()): boolean {
    if (file === BUILDER) return true;
    const memo = reaches.get(file);
    if (memo !== undefined) return memo;
    if (stack.has(file) || !existsSync(file)) return false;   // cycle, or not a repo file
    stack.add(file);
    let hit = false;
    for (const m of readFileSync(file, 'utf8').matchAll(SPEC)) {
        const target = resolveSpec(m[1], file);
        if (target && buildsWasm(target, stack)) { hit = true; break; }
    }
    stack.delete(file);
    reaches.set(file, hit);
    return hit;
}

/** Test files under a directory, non-recursively. */
function testFiles(dir: string): string[] {
    const full = join(REPO, dir);
    if (!existsSync(full)) return [];
    return readdirSync(full).filter((f) => f.endsWith('.test.ts')).map((f) => join(full, f));
}

const scripts = JSON.parse(readFileSync(join(REPO, 'package.json'), 'utf8')).scripts as Record<string, string>;

/**
 * The npm scripts that can end up building the host: the ones whose command
 * names a directory of test files where at least one file reaches BUILDER.
 * `run_e2e.mjs` runs the whole of e2e/, so it is treated as that directory.
 */
function wasmBuildingScripts(): string[] {
    const out: string[] = [];
    for (const [name, cmd] of Object.entries(scripts)) {
        const dirs = new Set<string>();
        for (const m of cmd.matchAll(/([\w./-]*e2e[\w./-]*)\/\*\.test\.ts/g)) dirs.add(m[1]);
        if (/run_e2e\.mjs/.test(cmd)) dirs.add('e2e');
        let hit = [...dirs].some((d) => testFiles(d).some((f) => buildsWasm(f)));
        if (!hit) {
            for (const m of cmd.matchAll(/\b(e2e\/[\w/-]+\.test\.ts)\b/g)) {
                if (buildsWasm(join(REPO, m[1]))) { hit = true; break; }
            }
        }
        if (hit) out.push(name);
    }
    return out;
}

// ---- the workflows -----------------------------------------------------------

interface Job { workflow: string; name: string; text: string }

/**
 * Split a workflow into jobs. Job keys sit at exactly two spaces under a
 * top-level `jobs:`; a job's text runs until the next such key.
 */
function jobsOf(workflow: string): Job[] {
    const lines = readFileSync(join(WORKFLOWS, workflow), 'utf8').split('\n');
    const start = lines.findIndex((l) => /^jobs:\s*$/.test(l));
    if (start < 0) return [];
    const out: Job[] = [];
    let current: Job | null = null;
    for (const line of lines.slice(start + 1)) {
        const key = line.match(/^ {2}([\w-]+):\s*$/);
        if (key) {
            if (current) out.push(current);
            current = { workflow, name: key[1], text: '' };
            continue;
        }
        if (current) current.text += `${line}\n`;
    }
    if (current) out.push(current);
    return out;
}

const jobs = readdirSync(WORKFLOWS)
    .filter((f) => f.endsWith('.yml') || f.endsWith('.yaml'))
    .flatMap(jobsOf);

const wasmScripts = wasmBuildingScripts();

/** The shared step that installs the toolchain (libclang, the pinned clang and binaryen). */
const PREBUILD = 'shared/scripts/ci_llvm.sh';

/**
 * Where a job first runs a suite that could build the host on demand, as an
 * offset into the job's text, or -1. A job may also build it, or a wasm module,
 * outright with `make ... table-host` / `make ... wasm-...`, which need the same
 * toolchain.
 */
function firstWasmUse(job: Job): number {
    const hits = [/\bmake\b[^\n]*\b(?:wasm[\w-]*|table-host)/, ...wasmScripts.map((s) => new RegExp(`npm run ${s.replace(/[:.]/g, '\\$&')}\\b`))]
        .map((rx) => job.text.search(rx))
        .filter((i) => i >= 0);
    return hits.length ? Math.min(...hits) : -1;
}

/** A job is covered when it runs the toolchain script before that first use. */
function prebuiltBefore(job: Job, use: number): boolean {
    const at = job.text.indexOf(PREBUILD);
    return at >= 0 && at < use;
}

test('the import graph really reaches the host builder (the gate has something to measure)', () => {
    // If this ever finds nothing, the two assertions below would pass vacuously
    // whatever CI looked like.
    assert.ok(existsSync(BUILDER), `${relative(REPO, BUILDER)} is gone - this gate needs rewriting`);
    const reaching = testFiles('e2e/validation').filter((f) => buildsWasm(f));
    assert.ok(reaching.length > 0,
        'no validation test reaches table_host.ts, so this gate would never fire');
    assert.ok(wasmScripts.length > 0, 'no npm script was found to build the host');
});

test('every CI job that can build the table host installs the toolchain first', () => {
    assert.ok(existsSync(join(resolve(REPO, '..'), PREBUILD)), `${PREBUILD} is gone - this gate needs rewriting`);

    const using = jobs.map((j) => ({ job: j, use: firstWasmUse(j) })).filter(({ use }) => use >= 0);
    assert.ok(using.length > 0, 'no CI job appears to build the host or a wasm target - did the workflows move?');

    const broken = using
        .filter(({ job, use }) => !prebuiltBefore(job, use))
        .map(({ job }) => `${job.workflow}:${job.name}`
            + (job.text.includes(PREBUILD) ? ' (runs the script, but AFTER the suite)' : ''))
        .sort();

    assert.deepEqual(broken, [],
        `these jobs reach a host or wasm build without running ${PREBUILD} first, so \`make\`\n`
        + 'runs on a runner with no libclang and every scenario in the lane fails on\n'
        + 'clang-c/Index.h rather than on anything it was written to test:\n'
        + `${broken.join('\n')}\n`
        + `(npm scripts that can build the host: ${wasmScripts.join(', ')})`);
});
