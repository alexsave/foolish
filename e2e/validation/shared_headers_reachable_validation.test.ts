// Every build system that compiles the kernel must be able to find the shared
// headers - and must need no flag of its own to do it.
//
// THE BUG THIS EXISTS FOR, in full, because it cost 8 red lanes. `sha256.h` and
// `deal_rng.h` moved to shared/c. `c/Makefile` grew `-I../shared/c` and every
// target in it went green, so the move looked finished. It was not: FIVE build
// systems compile these sources, and the other four had never heard of the flag.
//
//   c/Makefile                      the kernel, wasm, iOS, coverage, difftests
//   foolyard/Makefile               the discrete-event sim
//   server/impls/native/Makefile    the native server
//   rust/Makefile                   the C-side benches
//   werewolf/c/Makefile             the other product
//
// Two of them compiled `$(wildcard c/src/*.c)`, which is the nastier half: a
// wildcard does not fail when a file leaves it, it silently compiles less, and
// the build dies later at the link with an undefined symbol that names nothing
// about a move.
//
// THE FIX WAS TO MOVE THE INCLUDE, NOT TO ADD THE FLAG FOUR MORE TIMES. A
// quoted `#include` resolves against the directory of the file containing it
// BEFORE any -I, so `#include "../../shared/c/deal_rng.h"` in c/src/game.c needs
// nothing from anybody: any build system that can find game.c can find the
// header. That is one edit that fixes five build systems, instead of five edits
// that each have to be remembered again for the sixth.
//
// So this test guards the property that makes that work: no source may reach
// these headers through an include PATH, because an include path is per build
// system and is exactly what went wrong.
//
// Pure test - no Postgres, no network. It compiles, but only with -fsyntax-only
// and no -I at all, which is the whole point.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join, resolve, dirname, relative, normalize } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');

/** The headers that live in shared/ and are therefore reachable from nowhere by name. */
const SHARED_HEADERS = ['sha256.h', 'deal_rng.h'];

/** Every C source and header in the repo, both products, excluding build output. */
function kernelSources(): string[] {
    const out = execFileSync('git', ['ls-files', '*.c', '*.h'], { cwd: REPO, encoding: 'utf8' });
    return out.split('\n').filter((p) => p !== '' && !p.startsWith('shared/tools/'));
}

test('nothing reaches a shared header through an include path', () => {
    // A bare `#include "sha256.h"` compiles wherever some -I happens to cover
    // shared/c and fails everywhere else. Since the whole point is that no build
    // system carries that flag, a bare include is the bug, not a style choice.
    // Files inside shared/c itself are exempt: for them it IS the same directory.
    const offenders: string[] = [];
    for (const rel of kernelSources()) {
        if (rel.startsWith('shared/c/')) continue;
        const text = readFileSync(join(REPO, rel), 'utf8');
        text.split('\n').forEach((line, i) => {
            const m = line.match(/^\s*#\s*include\s*"([^"]+)"/);
            if (m && SHARED_HEADERS.includes(m[1])) {
                offenders.push(`${rel}:${i + 1}: ${line.trim()}`);
            }
        });
    }
    assert.deepEqual(offenders, [],
        'these include a shared header BY NAME, which only resolves where some\n'
        + 'build system happens to pass -I for shared/c. Five build systems compile\n'
        + 'this kernel and none of them passes one. Spell it relative to the file\n'
        + 'doing the including instead, e.g. "../../shared/c/sha256.h":\n  '
        + offenders.join('\n  '));
});

test('every relative include of a shared header resolves from its own file', () => {
    // The failure mode of the fix: right idea, wrong number of `../`. It shows up
    // only in whichever build system compiles that file, which may be the one
    // lane that needs a Linux container.
    const broken: string[] = [];
    const found: string[] = [];
    for (const rel of kernelSources()) {
        const text = readFileSync(join(REPO, rel), 'utf8');
        text.split('\n').forEach((line, i) => {
            const m = line.match(/^\s*#\s*include\s*"([^"]*\/)?([^"/]+)"/);
            if (!m || !SHARED_HEADERS.includes(m[2]) || !m[1]) return;
            const target = normalize(join(dirname(rel), m[1], m[2]));
            found.push(`${rel} -> ${target}`);
            if (!existsSync(join(REPO, target)) || !target.startsWith('shared/c/')) {
                broken.push(`${rel}:${i + 1}: "${m[1]}${m[2]}" -> ${target}`);
            }
        });
    }
    assert.ok(found.length >= 8,
        `expected the kernel to include the shared headers relatively in several places, found ${found.length}`);
    assert.deepEqual(broken, [],
        'these relative includes do not land on a file in shared/c - the number of\n'
        + '"../" is wrong for where the including file sits:\n  ' + broken.join('\n  '));
});

test('the shared sources compile with no -I whatsoever', () => {
    // The property stated as a compile rather than as a regex. If a TU that
    // includes a shared header compiles from the repo root with an EMPTY include
    // path, then it compiles under every build system's flags too, because none
    // of them can take an include path away.
    //
    // -fsyntax-only, so this needs no linker, no libm and no wasm toolchain.
    const probes = [
        'shared/c/sha256.c',
        'shared/c/deal_rng.c',
        'c/src/game.c',
        'werewolf/c/src/ww_game.c',
    ].filter((p) => existsSync(join(REPO, p)));
    assert.equal(probes.length, 4, 'a probe source is missing - did the kernel move again?');

    const failures: string[] = [];
    for (const rel of probes) {
        // Each product's own headers still come from its own -I; what must NOT be
        // needed is anything pointing at shared/. So the probe passes the source's
        // own directory and nothing else.
        const ownDir = join(REPO, dirname(rel));
        try {
            execFileSync('cc', ['-fsyntax-only', `-I${ownDir}`, '-D_Thread_local=',
                '-DACCELERATE_NEW_LAPACK', '-DCD_LEAFBOOK', join(REPO, rel)],
                { cwd: REPO, stdio: ['ignore', 'pipe', 'pipe'] });
        } catch (err) {
            const stderr = String((err as { stderr?: Buffer }).stderr ?? err);
            // Only a MISSING SHARED HEADER is this test's business. Any other
            // diagnostic belongs to whichever suite owns that file.
            for (const h of SHARED_HEADERS) {
                if (stderr.includes(`${h}' file not found`) || stderr.includes(`${h}: No such file`)) {
                    failures.push(`${rel}: cannot find ${h} without an extra -I\n${stderr.split('\n').slice(0, 4).join('\n')}`);
                }
            }
        }
    }
    assert.deepEqual(failures, [],
        'these need a build system to hand them an include path for shared/, which\n'
        + 'is the thing that broke 8 CI lanes:\n  ' + failures.join('\n  '));
});

test('every build system that compiles the kernel also compiles the shared sources', () => {
    // The other half of the move, and the half a wildcard hides. These builds
    // LINK deal_rng and sha256; a list that stopped naming them fails at the
    // link, far from the cause. Asserted by asking each Makefile what it thinks
    // its source list is, rather than by reading the list here.
    const builds: Array<{ make: string; variable: string }> = [
        { make: 'c', variable: 'CORE_SRC' },
        { make: 'foolyard', variable: 'KERNEL_SRC' },
        { make: 'server/impls/native', variable: 'KERNEL_SRC' },
        { make: 'werewolf/c', variable: 'CORE_SRC' },
    ];
    const missing: string[] = [];
    for (const { make, variable } of builds) {
        const dir = join(REPO, make);
        if (!statSync(dir).isDirectory()) { missing.push(`${make}: not a directory`); continue; }
        // A tiny makefile that includes theirs and prints the one variable, so
        // this reads the REAL list and not a copy of it.
        let printed: string;
        try {
            printed = execFileSync('make', ['-s', '-f', '-', 'sg-probe'], {
                cwd: dir, encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'],
                input: `include Makefile\nsg-probe:\n\t@echo $(${variable})\n`,
            });
        } catch (err) {
            missing.push(`${make}: could not read ${variable} (${String((err as { stderr?: Buffer }).stderr ?? err).split('\n')[0]})`);
            continue;
        }
        const list = printed.split(/\s+/).filter((s) => s !== '');
        for (const base of ['deal_rng.c', 'sha256.c']) {
            const hit = list.find((s) => s.endsWith(`/${base}`) || s === base);
            if (!hit) {
                missing.push(`${make}: ${variable} does not include ${base}`);
            } else if (!normalize(join(make, hit)).includes('shared/c/')) {
                missing.push(`${make}: ${variable} has ${hit}, which is not the shared copy`);
            }
        }
    }
    assert.deepEqual(missing, [],
        'these build systems link the kernel but no longer compile the shared half.\n'
        + 'Two of them use $(wildcard c/src/*.c), which does not fail when a file\n'
        + 'leaves it - it just compiles less, and dies at the link:\n  '
        + missing.join('\n  '));
});

// ---- the source list the freshness gate is built from ------------------------

test('the wasm source list is the same list when make is the caller', () => {
    // WHY THE ENVIRONMENT IS THE TEST. scripts/wasm_stamp.sh asks the Makefiles
    // what the wasm sources are. `make -s` silences recipes but NOT "make[1]:
    // Entering directory '...'", and make prints those whenever MAKELEVEL is set
    // - which it is when c/Makefile calls the script FROM A RECIPE, and is not
    // when a person runs it in a terminal.
    //
    // So the script produced a clean list every time I ran it and a list with
    // five lines of English in it every time CI did. Every path here is compared
    // against `git diff --name-only` by check_wasm_freshness.sh, so junk in this
    // list is a gate reasoning about files that do not exist.
    //
    // Running it BOTH ways and demanding the same answer is the cheapest way to
    // stop that being a CI-only discovery.
    const run = (env: NodeJS.ProcessEnv) =>
        execFileSync('bash', [join(REPO, 'scripts/wasm_stamp.sh'), '--list'],
            { cwd: REPO, encoding: 'utf8', env }).trim().split('\n');

    const plain = run({ ...process.env, MAKELEVEL: undefined, MAKEFLAGS: undefined });
    const underMake = run({ ...process.env, MAKELEVEL: '1', MAKEFLAGS: 'w' });

    assert.ok(plain.length > 50, `the source list came back with only ${plain.length} entries`);
    assert.deepEqual(underMake, plain,
        'scripts/wasm_stamp.sh lists different sources depending on whether make is\n'
        + 'its caller. That is "make[1]: Entering directory" landing in the list -\n'
        + 'add --no-print-directory to the make call that grew it.');

    const missing = plain.filter((p) => !existsSync(join(REPO, p)));
    assert.deepEqual(missing, [], `these listed wasm sources do not exist:\n  ${missing.join('\n  ')}`);
});
