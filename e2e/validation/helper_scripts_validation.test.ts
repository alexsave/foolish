// A helper script may not name a file this repo does not have.
//
// `scripts/` and `c/tools/` hold the tools nobody runs on a schedule: the
// one-off decoders, the analysis pipelines, the regenerators. Nothing in CI
// executes them, so a rename in the kernel or a directory move under server/
// orphans one in total silence. It has happened repeatedly:
//
//   - A10 moved the backend to server/impls/supabase/ and the repo-root
//     supabase/ tree went away. Three og_explain drivers kept swapping the
//     instrumented wasm into supabase/functions/_shared/wasm/bots.wasm.gz -
//     first a path no loader read (a SILENT wrong answer: the drive ran the
//     shipped, uninstrumented bot and dumped nothing), then a path that was not
//     there at all.
//   - The same pipeline kept shelling out to e2e/_wasm_drive.test.ts,
//     e2e/_wasm_multi_drive.test.ts and two more that had been deleted.
//   - "reading a pasted link is the kernel's too" moved urlToGame out of
//     server/api/common/replay/codec.ts into kernelReplayLinkParse. Two scripts
//     went on destructuring the old name and threw on their first line.
//   - A10 and the kernel lift deleted server/api/common/game_lifecycle.ts,
//     sdk/ts/wasm/engine.ts and four more modules that helper scripts import.
//
// Every one of those is the same defect: a helper names a path, and nothing
// checks the path. So this checks them, in the two forms a helper names a file.
//
//   1. MODULE IMPORTS - every relative or aliased specifier must resolve to a
//      file that exists, and every NAMED binding must really be exported by it
//      (re-exports followed), so a rename inside a surviving module is caught
//      too, not just a deleted file.
//   2. REPO PATH LITERALS - a quoted string that looks like a repo-relative
//      source or asset path, plus python's os.path.join(ROOT, 'a', 'b') form,
//      must point at something that exists.
//
// The set of scripts is walked, not listed, so a new helper is covered the day
// it lands. A helper that is deliberately kept but no longer works says so in
// its own header with `@archived: <reason>`; a single line that names something
// outside the repo (a build tree, say) says `no-repo-path` on that line. Both
// are marks on the file that owns the problem, not entries in a list here.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');

/* ---------------------------------------------------------------------------
 * What the build provides
 * ---------------------------------------------------------------------------
 * sdk/ts/gen and its siblings stopped being committed (#174), so on a fresh
 * checkout they are simply absent - and scripts/decode_replay.mjs imports one
 * of them. "The file is not there" is the right answer for a source module and
 * the wrong one for a build output, so those directories are held to be
 * PROVIDED rather than checked for existence.
 *
 * It is deliberately not "skip anything that does not resolve": the target must
 * land inside a directory gen.sh declares it writes into, so a typo one
 * character outside one (sdk/ts/genx/...) still fails.
 *
 * The list comes from `gen.sh --print-dirs`, the same derivation
 * generated_outputs_validation.test.ts uses, so there is one answer in the repo
 * to "what is generated". Two things this deliberately does NOT do:
 *
 *   - it does not run the generator. This lane is the pure one; structgen is a
 *     libclang build, and a pure test that shells out to a compiler is the
 *     defect ci_toolchain_validation.test.ts exists to catch.
 *   - it does not check the file when it happens to be present. `npm run
 *     test:validate` has a pretest hook that generates, so on that path these
 *     files DO exist - and a test whose verdict depends on whether a build
 *     happened to run is a test that is green here and red on a fresh runner.
 *     The same answer either way is worth more than the extra coverage.
 *
 * A rename INSIDE a generated module is caught where it belongs, by
 * `gen.sh --check` and the structgen validation, not here.
 */
function generatedRoots(): string[] {
    let printed: string;
    try {
        printed = execFileSync('bash', [join(REPO, 'tools/structgen/gen.sh'), '--print-dirs'],
            { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    } catch {
        return [];      // the assertion below says so, rather than a load-time throw
    }
    return printed.split('\n').map((s) => s.trim()).filter(Boolean).map((d) => resolve(REPO, d));
}
const GENERATED_ROOTS: string[] = generatedRoots();

/** Is this path a build output some lane generates, rather than a source file? */
function isGenerated(abs: string): boolean {
    return GENERATED_ROOTS.some((root) => abs === root || abs.startsWith(root + '/'));
}

/** The tool directories: everything here is run by hand, never by CI. */
const ROOTS = ['scripts', 'c/tools'];

/** Directories that hold no source of ours. */
const SKIP_DIRS = new Set(['node_modules', '.build', '.git']);

const CODE = /\.(mjs|mts|cjs|js|ts|tsx|py|sh)$/;

/** Generated trees. A literal rooted here names a build product, not a source. */
const GENERATED = /^(build|dist|out|target|coverage|node_modules|\.next|\.build)$/;

/** `@archived: <reason>` in the header keeps a deliberately-dead helper quiet. */
const ARCHIVED = /@archived:\s*(.+)$/m;

/** `no-repo-path` on a line excuses the path literals on that line. */
const NO_PATH = /no-repo-path/;

function walk(dir: string, out: string[] = []): string[] {
    for (const entry of readdirSync(dir)) {
        if (SKIP_DIRS.has(entry)) continue;
        const p = join(dir, entry);
        if (statSync(p).isDirectory()) walk(p, out);
        else if (CODE.test(entry)) out.push(p);
    }
    return out;
}

/** Every helper under ROOTS, repo-relative, with the archived ones split off. */
function helpers(): { live: string[]; archived: { file: string; reason: string }[] } {
    const live: string[] = [];
    const archived: { file: string; reason: string }[] = [];
    for (const root of ROOTS) {
        for (const abs of walk(join(REPO, root))) {
            const file = relative(REPO, abs);
            const head = readFileSync(abs, 'utf8').split('\n').slice(0, 40).join('\n');
            const mark = head.match(ARCHIVED);
            if (mark) archived.push({ file, reason: mark[1].trim() });
            else live.push(file);
        }
    }
    return { live: live.sort(), archived: archived.sort((a, b) => a.file.localeCompare(b.file)) };
}

/* ---------------------------------------------------------------------------
 * 1. Module imports
 * ------------------------------------------------------------------------ */

/** The `paths` aliases, read from the tsconfig rather than copied into here. */
function aliases(): [string, string][] {
    const raw = readFileSync(join(REPO, 'tsconfig.json'), 'utf8')
        .replace(/^\s*\/\/.*$/gm, '');
    const paths = JSON.parse(raw).compilerOptions.paths as Record<string, string[]>;
    const out: [string, string][] = [];
    for (const [pattern, targets] of Object.entries(paths)) {
        if (!pattern.endsWith('/*') || !targets[0]?.endsWith('/*')) continue;
        out.push([pattern.slice(0, -1), targets[0].slice(0, -1).replace(/^\.\//, '')]);
    }
    assert.ok(out.length > 0, 'tsconfig.json declares no path aliases - did they move?');
    return out;
}
const ALIASES = aliases();

/** A specifier that names a file in this repo, or null for node:/npm:/bare. */
function resolveSpec(spec: string, fromFile: string): string | null {
    for (const [alias, target] of ALIASES) {
        if (spec.startsWith(alias)) return resolve(REPO, target, spec.slice(alias.length));
    }
    if (spec.startsWith('.')) return resolve(dirname(resolve(REPO, fromFile)), spec);
    return null;
}

interface Imported { spec: string; target: string | null; names: string[] }

/** `import {a, b as c} from '...'`, and the `const {a} = await import(\`${R}/x\`)`
 *  form the .mjs helpers use to reach a TS module from plain JavaScript. */
function importsOf(file: string): Imported[] {
    const txt = readFileSync(join(REPO, file), 'utf8');
    const out: Imported[] = [];

    const named = (clause: string): string[] => {
        const braces = clause.match(/\{([\s\S]*)\}/);
        if (!braces) return [];
        return braces[1].split(',')
            .map((s) => s.trim().split(/\s+as\s+|\s*:\s*/)[0].trim())
            .filter((s) => s.length > 0 && s !== 'type');
    };

    for (const m of txt.matchAll(/(?:^|\n)\s*(?:import|export)\s+([\s\S]*?)\s*from\s*['"]([^'"]+)['"]/g)) {
        out.push({ spec: m[2], target: resolveSpec(m[2], file), names: named(m[1]) });
    }
    for (const m of txt.matchAll(/(?:const|let|var)\s*(\{[^}]*\})\s*=\s*await\s+import\(\s*`\$\{[A-Za-z_$][\w$]*\}\/([^`]+)`\s*\)/g)) {
        out.push({ spec: m[2], target: resolve(REPO, m[2]), names: named(m[1]) });
    }
    return out;
}

/** Does `mod` export `name`? Follows `export * from './x'` one module at a time. */
function exportsName(mod: string, name: string, seen = new Set<string>()): boolean {
    if (seen.has(mod) || !existsSync(mod)) return false;
    seen.add(mod);
    const txt = readFileSync(mod, 'utf8');
    const decl = new RegExp(
        `export\\s+(?:declare\\s+)?(?:async\\s+)?(?:default\\s+)?` +
        `(?:function\\*?|const|let|var|class|type|interface|enum)\\s+${name}\\b`);
    if (decl.test(txt)) return true;
    // `export { a, b as name }` - the exported name is the one after `as`.
    for (const m of txt.matchAll(/export\s*\{([^}]*)\}/g)) {
        for (const part of m[1].split(',')) {
            const bits = part.trim().split(/\s+as\s+/);
            if ((bits[1] ?? bits[0]).trim() === name) return true;
        }
    }
    for (const m of txt.matchAll(/export\s*\*\s*from\s*['"]([^'"]+)['"]/g)) {
        const next = resolveSpec(m[1], relative(REPO, mod));
        if (next && exportsName(next, name, seen)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * 2. Repo path literals
 * ------------------------------------------------------------------------ */

/** A quoted `dir/some/file.ext`, in any of the four languages here. */
const PATH_LITERAL = /['"`]([a-z][a-z0-9_]*(?:\/[A-Za-z0-9_.\[\]-]+)+)['"`]/g;
const PATH_EXT = /\.(ts|tsx|mts|mjs|cjs|js|py|sh|c|h|json|wasm|gz|html|md|toml|yml|yaml|swift)$/;
/** `os.path.join(ROOT, 'a', 'b')` with every segment a literal. */
const PY_JOIN = /os\.path\.join\(\s*[A-Za-z_$][\w$]*\s*,\s*((?:'[^']+'\s*,\s*)*'[^']+')\s*\)/g;

function pathLiterals(file: string): string[] {
    const hits = new Set<string>();
    for (const line of readFileSync(join(REPO, file), 'utf8').split('\n')) {
        if (NO_PATH.test(line)) continue;
        for (const m of line.matchAll(PATH_LITERAL)) {
            if (PATH_EXT.test(m[1])) hits.add(m[1]);
        }
        for (const m of line.matchAll(PY_JOIN)) {
            const segs = [...m[1].matchAll(/'([^']+)'/g)].map((s) => s[1]);
            if (segs.length > 1 && /^[a-z][a-z0-9_]*$/.test(segs[0])) hits.add(segs.join('/'));
        }
    }
    return [...hits].filter((p) => !GENERATED.test(p.split('/')[0]));
}

/* ---------------------------------------------------------------------------
 * The gates
 * ------------------------------------------------------------------------ */

const { live, archived } = helpers();

test('every module a helper script imports still exists', () => {
    const broken: string[] = [];
    for (const file of live) {
        for (const imp of importsOf(file)) {
            if (!imp.target || isGenerated(imp.target)) continue;
            if (!existsSync(imp.target)) broken.push(`${file} imports '${imp.spec}'`);
        }
    }
    assert.deepEqual(broken, [],
        'these helper scripts import modules that are not in the repo - they throw on their '
        + 'first line and no test runs them:\n' + broken.join('\n'));
});

test('every name a helper script imports is still exported', () => {
    const broken: string[] = [];
    for (const file of live) {
        for (const imp of importsOf(file)) {
            if (!imp.target || isGenerated(imp.target) || !existsSync(imp.target)) continue;
            for (const name of imp.names) {
                if (!exportsName(imp.target, name)) {
                    broken.push(`${file}: '${imp.spec}' no longer exports ${name}`);
                }
            }
        }
    }
    assert.deepEqual(broken, [],
        'these helper scripts destructure names their module stopped exporting - a rename '
        + 'orphaned them and nothing objected:\n' + broken.join('\n'));
});

test('every repo path a helper script names still exists', () => {
    const broken: string[] = [];
    for (const file of live) {
        for (const p of pathLiterals(file)) {
            const abs = join(REPO, p);
            if (isGenerated(abs)) continue;         // a build output, not a source
            if (!existsSync(abs)) broken.push(`${file} names '${p}'`);
        }
    }
    assert.deepEqual(broken, [],
        'these helper scripts name repo paths that do not exist - a directory move left them '
        + 'behind:\n' + broken.join('\n')
        + '\n(a literal that means a path OUTSIDE the repo says `no-repo-path` on its line)');
});

// The generated-roots exemption is the one place a path is allowed not to
// exist, so it has to stay exactly as wide as gen.sh says and no wider. A
// --print-dirs that started answering with nothing, or a prefix test that let
// a neighbouring directory through, would quietly turn the first two gates off.
test('the generated-output exemption is only as wide as gen.sh declares', () => {
    assert.ok(GENERATED_ROOTS.length >= 3,
        `gen.sh --print-dirs answered with ${GENERATED_ROOTS.length} directories - `
        + 'an empty answer would exempt nothing, a wrong one could exempt the repo');
    for (const root of GENERATED_ROOTS) {
        assert.ok(root.startsWith(REPO + '/'), `generated root outside the repo: ${root}`);
        assert.equal(isGenerated(join(root, 'a/module.ts')), true, `${root} does not exempt its own files`);
        // A sibling one character wider is a different directory, not this one.
        assert.equal(isGenerated(root + 'x/module.ts'), false, `${root}x leaked into the exemption`);
    }
    assert.equal(isGenerated(join(REPO, 'sdk/ts/wasm/bots.ts')), false,
        'a hand-written module is being treated as generated');
});

test('an archived helper says why it is kept', () => {
    const silent = archived.filter((a) => a.reason.length < 20).map((a) => a.file);
    assert.deepEqual(silent, [],
        `@archived needs a reason a reader can act on: ${silent.join(', ')}`);
});

// The three gates above pass trivially if the walk finds nothing, if the import
// regexes stop matching, or if every path literal is filtered away. Each of
// those is a silent hole exactly like the one this file exists to close, so the
// scan's own reach is asserted too.
test('the scan actually reaches the helper scripts and what they name', () => {
    assert.ok(live.length >= 20, `walked only ${live.length} helper scripts - did ROOTS move?`);

    const withImports = live.filter((f) => importsOf(f).some((i) => i.target !== null));
    assert.ok(withImports.length >= 2,
        `no helper script appears to import a repo module (${withImports.length}) - `
        + 'the import regexes have stopped matching');

    const names = withImports.flatMap((f) => importsOf(f).flatMap((i) => (i.target ? i.names : [])));
    assert.ok(names.length >= 5,
        `extracted only ${names.length} named bindings - the destructuring regex has rotted`);

    const paths = live.flatMap((f) => pathLiterals(f));
    assert.ok(paths.length >= 8,
        `extracted only ${paths.length} repo path literals - the literal regex has rotted`);
});

// A scanner that never reports anything is the same hole again, so each half is
// shown to fire on a file that really is broken. The fixtures are strings, not
// files, so nothing on disk has to stay broken for this to hold.
test('the scan reports a break when there is one', () => {
    const probe = 'e2e/validation/helper_scripts_validation.test.ts';

    // A module that is not there.
    assert.equal(resolveSpec('../../server/api/common/gone_module.ts', probe) !== null, true);
    assert.equal(existsSync(resolveSpec('../../server/api/common/gone_module.ts', probe)!), false);

    // A module that is there but no longer exports the name.
    const real = resolveSpec('../../sdk/ts/wasm/bots.ts', probe)!;
    assert.equal(existsSync(real), true, 'sdk/ts/wasm/bots.ts moved - repoint this probe');
    assert.equal(exportsName(real, 'kernelReplayLinkParse'), true, 'the export check cannot see a real export');
    assert.equal(exportsName(real, 'urlToGame'), false, 'the export check claims a departed name is exported');

    // A path literal that names nothing.
    assert.equal(existsSync(join(REPO, 'supabase/functions/_shared/wasm/bots.wasm.gz')), false,
        'the A10 orphan is back - this probe needs a different dead path');
});
