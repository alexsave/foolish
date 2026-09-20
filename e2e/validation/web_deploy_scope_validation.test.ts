// Nothing the web bundle is built from may sit under a path the deploy filter skips.
//
// Vercel's free plan allows 100 DEPLOYMENTS per day across the account, and on
// 2026-09-18 this repo spent all of them before the evening, after which every
// deploy failed - production included. The cause was that `.github/workflows/
// web.yml`'s deploy job ran on every push to every pull request, so a day of C,
// docs and iOS work burned the same budget as web work.
//
// scripts/web_deploy_scope.sh now decides whether a change can reach the served
// site. That decision is the dangerous kind: when it says "skip" and is WRONG,
// nothing fails. The PR merges green, no preview is built, and after a merge to
// main the site keeps serving the old bundle. It is the same silent-staleness
// shape that let deploy.yml's trigger watch a path that had stopped existing
// (see deploy_paths_validation.test.ts) while every check stayed green.
//
// A hand-maintained list would rot the same way, so this derives the answer: it
// walks the web's REAL import graph from the Next.js entrypoints, resolving the
// tsconfig aliases the bundler resolves, and asserts that nothing it reaches
// lives under a skipped prefix.
//
// The live example of why: tsconfig.json aliases `@shared/*` into
// server/impls/supabase/functions/_shared/, and `^server/impls/` IS skipped.
// That is correct only while no web source imports through that alias. One
// `import { x } from '@shared/y'` in src/ would make the skip wrong, and this
// test is the only thing that would say so.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const FILTER = join(REPO, 'scripts/web_deploy_scope.sh');
const WORKFLOW = join(REPO, '.github/workflows/web.yml');

/** The skip patterns, read out of the filter itself so the two cannot disagree. */
function skipPatterns(): RegExp[] {
    const src = readFileSync(FILTER, 'utf8');
    const m = src.match(/^SKIP='([^']*)'/m);
    assert.ok(m, `could not find the SKIP list in ${relative(REPO, FILTER)} - did its shape change?`);
    const lines = m![1].split('\n').map((l) => l.trim()).filter(Boolean);
    assert.ok(lines.length > 3, 'the SKIP list looks empty; the parse is probably wrong');
    return lines.map((rx) => new RegExp(rx));
}

/** tsconfig's path aliases, which are what the bundler resolves. */
function aliases(): Record<string, string> {
    // Parse as-is first. Stripping comments naively is not safe here: the alias
    // keys themselves contain `/*` (`"@sdk/*"`), and a block-comment regex eats
    // from there to the next `*/`, which silently mangles the alias table.
    const raw = readFileSync(join(REPO, 'tsconfig.json'), 'utf8');
    let parsed: { compilerOptions?: { paths?: Record<string, string[]> } };
    try {
        parsed = JSON.parse(raw);
    } catch {
        // tsconfig allows comments; drop only whole-line ones, which cannot
        // occur inside a string on their own line.
        parsed = JSON.parse(raw.replace(/^\s*\/\/.*$/gm, ''));
    }
    const paths = parsed.compilerOptions?.paths ?? {};
    const out: Record<string, string> = {};
    for (const [alias, targets] of Object.entries(paths as Record<string, string[]>)) {
        out[alias.replace(/\*$/, '')] = resolve(REPO, targets[0].replace(/\*$/, ''));
    }
    assert.ok(Object.keys(out).length > 0, 'tsconfig.json declares no path aliases - did it move?');
    return out;
}

/** Every quoted specifier in `from '...'`, `import('...')` and `new URL('...')`. */
const SPEC = /(?:from|import|URL)\s*\(?\s*['"]([^'"]+)['"]/g;

const ALIASES = aliases();

/** The candidate files a specifier could name, in resolution order. */
function candidates(spec: string, fromFile: string): string[] {
    let base: string | null = null;
    for (const [alias, target] of Object.entries(ALIASES)) {
        if (spec.startsWith(alias)) { base = resolve(target, spec.slice(alias.length)); break; }
    }
    if (base === null && spec.startsWith('.')) base = resolve(dirname(fromFile), spec);
    if (base === null) return [];   // a bare package name - node_modules, not ours
    // `allowImportingTsExtensions` means most specifiers already carry .ts/.tsx.
    return [base, `${base}.ts`, `${base}.tsx`, join(base, 'index.ts'), join(base, 'index.tsx')];
}

/** Next.js compiles every special file under src/app as an entrypoint. */
function entrypoints(): string[] {
    const ENTRY = /^(page|layout|route|template|error|not-found|loading|global-error)\.(tsx?|jsx?)$/;
    const out: string[] = [];
    const walk = (dir: string) => {
        for (const name of readdirSync(dir)) {
            const p = join(dir, name);
            if (statSync(p).isDirectory()) walk(p);
            else if (ENTRY.test(name)) out.push(p);
        }
    };
    walk(join(REPO, 'src/app'));
    assert.ok(out.length > 0, 'found no Next.js entrypoints under src/app - did the layout move?');
    return out;
}

/** Everything the bundle reaches, transitively. */
function bundleGraph(): string[] {
    const seen = new Set<string>();
    const queue = entrypoints();
    while (queue.length) {
        const f = queue.pop()!;
        if (seen.has(f) || !existsSync(f) || !statSync(f).isFile()) continue;
        seen.add(f);
        for (const m of readFileSync(f, 'utf8').matchAll(SPEC)) {
            for (const c of candidates(m[1], f)) {
                if (existsSync(c) && statSync(c).isFile() && !seen.has(c)) { queue.push(c); break; }
            }
        }
    }
    return [...seen].map((p) => relative(REPO, p));
}

test('nothing the web bundle imports lives under a path the deploy filter skips', () => {
    const skips = skipPatterns();
    const reached = bundleGraph();
    assert.ok(reached.length > 20, `only ${reached.length} files reached - the walk is probably broken`);

    const wrong = reached.filter((p) => skips.some((rx) => rx.test(p)));
    assert.deepEqual(wrong, [], [
        'These files are compiled into the web bundle but sit under a prefix that',
        'scripts/web_deploy_scope.sh treats as unable to reach the site. A change to',
        'one of them would merge without a preview, and main would not redeploy:',
        ...wrong.map((p) => `  ${p}`),
        '',
        'Either stop importing them from the web, or remove that prefix from SKIP.',
    ].join('\n'));
});

test('the filter never skips anything that can change what a visitor downloads', () => {
    // The import-graph test above only sees what the bundle IMPORTS. Three kinds
    // of input reach a visitor without ever being imported, so they need naming:
    //
    //   c/**            sdk/ts/gen/* is GENERATED from these headers by
    //                   tools/structgen, and sdk/ts/wasm/bots.wasm.gz is built
    //                   from c/src. Neither is committed, so neither shows up as
    //                   a file the graph walk can reach.
    //   tools/structgen the generator itself: change what it emits and the
    //                   bundle changes without a single import moving.
    //   public/**       served verbatim by Next.js, never imported.
    //
    // Each was found by mutation: adding public/ or tools/ to SKIP passed every
    // other test in this file.
    const skips = skipPatterns();
    const never = [
        'c/src/game.c', 'c/src/legal.h', 'c/wasm/wasm_api.c',
        'sdk/ts/wasm/bots.wasm.gz', 'sdk/ts/wire/awire.ts',
        'shared/tools/structgen/structgen.c', 'tools/structgen/gen.sh',
        'tools/structgen/specs/view_layout.args',
        'public/oracle.wasm.gz', 'public/favicon.ico',
        'src/app/page.tsx', 'package.json', 'package-lock.json',
        'next.config.ts', 'tsconfig.json',
    ];
    const skipped = never.filter((p) => skips.some((rx) => rx.test(p)));
    assert.deepEqual(skipped, [], [
        'These can change what a visitor downloads, but the filter would skip them,',
        'so a change to one would merge with no preview and never redeploy main:',
        ...skipped.map((p) => `  ${p}`),
    ].join('\n'));
});

test('the workflow actually uses the filter, and production is never scoped', () => {
    const wf = readFileSync(WORKFLOW, 'utf8');
    assert.match(wf, /bash scripts\/web_deploy_scope\.sh/,
        'web.yml does not call scripts/web_deploy_scope.sh - the filter is tested but unused');
    assert.match(wf, /needs\.scope\.outputs\.deploy == 'true'/,
        'the deploy job is not gated on the scope job');
    // A merge to main must deploy whatever changed: a stale production site costs
    // more than a deployment, and main merges are a couple of dozen a day.
    assert.match(wf, /github\.event_name.*!=.*pull_request[\s\S]{0,200}deploy=true/,
        'web.yml no longer deploys production unconditionally');
});

test('previews are opt-in, and asking for one actually triggers a run', () => {
    // Owner decision 2026-09-18, after production could not deploy for hours
    // because preview deploys had spent the account's 100/day: "make previews
    // opt in. Most changes don't affect web lately."
    const wf = readFileSync(WORKFLOW, 'utf8');

    assert.match(wf, /contains\(github\.event\.pull_request\.labels\.\*\.name, 'preview'\)/,
        'web.yml no longer gates the preview on the `preview` label - every pull request '
        + 'will deploy again, and production loses the race for the day\'s budget');

    // THIS ASSERTION EXISTS BECAUSE THE BUG SHIPPED. The first version of the
    // label gate wrote ''preview'' inside a `run: |` block, doubling the quotes
    // as if it were a YAML single-quoted scalar. A block scalar does not quote
    // anything, so Actions received contains(..., ''preview'') , rejected the
    // expression, and rejected the WHOLE WORKFLOW FILE - every job vanished and
    // main's production deploy never ran.
    //
    // Nothing local caught it: the file is valid YAML (ruby -ryaml parses it
    // happily) and merely an invalid Actions expression, and the test above was
    // written to match the broken spelling, so it went green against the defect
    // it was supposed to describe. Same failure family as every other entry in
    // this repo's mutation notes - green against the wrong artifact.
    for (const [bad, what] of [
        ["''preview''", 'doubled single quotes (YAML-scalar escaping inside a block scalar)'],
        ['\\"preview\\"', 'escaped double quotes'],
    ] as const) {
        assert.equal(wf.includes(`labels.*.name, ${bad}`), false,
            `web.yml uses ${what} in the label expression. GitHub will refuse the whole `
            + 'workflow file, not just this step, and every job in it disappears.');
    }

    // The label must be a TRIGGER, not only a condition. Without `labeled` in
    // the event types, adding the label does nothing until the next push: you
    // ask for a preview, nothing happens, and no message says why.
    const on = wf.slice(0, wf.indexOf('jobs:'));
    assert.match(on, /types:\s*\[[^\]]*labeled[^\]]*\]/,
        'pull_request does not trigger on `labeled`, so adding the preview label '
        + 'would not build a preview until someone pushed again');

    // Production must not be reachable through the label gate: the unconditional
    // production path has to come FIRST, before any label check, or a merge to
    // main would need a label it can never carry.
    const decide = wf.slice(wf.indexOf('id: decide'));
    const prodAt = decide.search(/github\.event_name.*!=.*pull_request/);
    const labelAt = decide.search(/contains\(github\.event\.pull_request\.labels/);
    assert.ok(prodAt >= 0 && labelAt >= 0 && prodAt < labelAt,
        'the label gate now runs before the production branch - a merge to main would '
        + 'be asked for a label it cannot have, and production would never deploy');
});

test('the filter is executable and self-consistent', () => {
    assert.ok(existsSync(FILTER), 'scripts/web_deploy_scope.sh is missing');
    // eslint-disable-next-line no-bitwise
    assert.ok(statSync(FILTER).mode & 0o111, 'scripts/web_deploy_scope.sh is not executable');
    const skips = skipPatterns();
    // The filter re-admits these two by name; a SKIP entry that also matched them
    // would make that re-admission dead code and the lane untestable in a PR.
    for (const p of ['.github/workflows/web.yml', 'scripts/web_deploy_scope.sh']) {
        assert.ok(readFileSync(FILTER, 'utf8').includes(p), `${p} is no longer re-admitted by name`);
    }
});
