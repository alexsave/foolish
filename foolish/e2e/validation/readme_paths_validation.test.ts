// The front page may not name a path that is not there.
//
// README.md is the one file every reader opens first and nobody's build ever
// touches, which is the exact combination that rots. The version this replaced
// had, on the repo's front page: a `supabase/` tree that had moved to
// server/impls/, a `rustpoc/` that had been renamed `rust/`, a `migrations/`
// directory that had been folded into seed.sql, "three languages (en/ru/ko)"
// when there were twenty-five, "deploys to Vercel with zero config" when the
// Git integration had been deliberately disconnected, and - for several months,
// at the top of the page - literal tool-call scaffolding (`</content>`,
// `</invoke>`) left behind by whatever wrote it.
//
// Every one of those was wrong for weeks and no check anywhere noticed, because
// prose is not compiled. This compiles the part of it that can be: the paths.
// It is the same argument helper_scripts_validation.test.ts makes for scripts
// ("every repo path a helper script names still exists"), pointed at the
// document that is read the most and verified the least.
//
// It deliberately does NOT try to check the claims. A sentence saying the
// kernel is C is beyond this, and so is a line count going stale. What it can
// say is that a reader who follows a link, or looks for a directory the map
// draws, finds something there.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, resolve } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const README = join(REPO, 'README.md');

const text = () => readFileSync(README, 'utf8');

test('every relative link in the README resolves to something that exists', () => {
    // `[label](target)`, minus anything with a scheme and minus bare anchors.
    const targets = [...text().matchAll(/\]\(([^)\s]+)\)/g)]
        .map((m) => m[1])
        .filter((t) => !/^[a-z][a-z0-9+.-]*:/i.test(t) && !t.startsWith('#'))
        .map((t) => t.split('#')[0])
        .filter(Boolean);

    assert.ok(targets.length > 10,
        `only ${targets.length} relative links parsed out of README.md - the parse is probably wrong`);

    const dead = [...new Set(targets)].filter((t) => !existsSync(join(REPO, t)));
    assert.deepEqual(dead, [], [
        'README.md links to these, and they are not in the repo:',
        ...dead.map((t) => `  ${t}`),
        '',
        'Either the file moved and the link needs following, or the link describes',
        'a layout that stopped being true.',
    ].join('\n'));
});

test('every directory the README map draws is a directory that exists', () => {
    // The fenced block under "## The map". Its shape is a path, whitespace, and
    // prose; nested entries are indented under an unindented parent, which is
    // what the indentation MEANS, so that is how they resolve.
    const block = text().match(/## The map\s*\n+```\n([\s\S]*?)\n```/);
    assert.ok(block, 'could not find the fenced map block under "## The map" in README.md');

    const drawn: string[] = [];
    let parent = '';
    for (const line of block![1].split('\n')) {
        // A path token: first word on the line, containing or ending with a
        // slash. Prose continuation lines are indented past any path column and
        // start with a letter that is part of a sentence, so requiring the
        // slash is what separates them.
        const m = line.match(/^(\s*)([A-Za-z0-9_.-]+\/[A-Za-z0-9_./-]*)\s{2,}/);
        if (!m) continue;
        const [, indent, p] = m;
        if (indent.length === 0) { parent = p; drawn.push(p); continue; }
        // Indented: under the current parent, unless it already resolves alone
        // (the map writes a few, like `impls/supabase/`, as parent-relative and
        // a few, like `Tools/rig/`, the same way).
        drawn.push(existsSync(join(REPO, p)) ? p : join(parent, p));
    }

    assert.ok(drawn.length > 15,
        `only ${drawn.length} paths parsed out of the map block - the parse is probably wrong`);

    const missing = [...new Set(drawn)].filter((p) => !existsSync(join(REPO, p)));
    assert.deepEqual(missing, [], [
        'The README map draws these, and they are not in the repo:',
        ...missing.map((p) => `  ${p}`),
        '',
        'A map of a tree that moved is worse than no map: it sends every new reader',
        'to the wrong place, confidently.',
    ].join('\n'));
});

test('no tool-call scaffolding survives into the front page', () => {
    // This happened. `</content>` and `</invoke>` sat at the bottom of the repo's
    // front page from commit fcfb6b88 until the September 2026 doc audit found
    // them - visible to anyone who opened the project, invisible to everything
    // that runs.
    const junk = ['</content>', '</invoke>', '<invoke', '<parameter', '</antml'];
    const found = junk.filter((j) => text().includes(j));
    assert.deepEqual(found, [], [
        'README.md contains tool-call scaffolding:',
        ...found.map((j) => `  ${j}`),
    ].join('\n'));
});
