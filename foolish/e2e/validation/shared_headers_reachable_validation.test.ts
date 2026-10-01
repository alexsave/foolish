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

// This gate spans the products, so it is rooted at the REPOSITORY, where foolish/,
// werewolf/ and shared/ sit side by side; PRODUCT is foolish/ itself, which is
// where scripts/wasm_stamp.sh runs and what its paths are relative to.
const PRODUCT = resolve(import.meta.dirname, '../..');
const REPO = resolve(PRODUCT, '..');

/**
 * The headers that live in shared/ and are therefore reachable from nowhere by name.
 * Every header under shared/c is here, whoever reaches it today:
 *
 *   sha256.h, deal_rng.h   the first two, and the reason this test exists
 *   b32.h, mixrad.h        the code alphabet and the mixed-radix arithmetic
 *                          under the game coders, lifted after the first two
 *                          and included by four products the same relative way
 *   languages.h            shared/c/i18n/languages.h, the language registry; a
 *                          kernel includes it as "../../../shared/c/i18n/languages.h"
 *   msg_stage.h,           reached from Swift through each one's module.modulemap
 *   motion_ruler.h         on SWIFT_INCLUDE_PATHS (the one sanctioned include path,
 *                          because Swift has no relative #include). No product C
 *                          file includes either today; listing them makes the
 *                          first one that does spell it relatively.
 *   check.h, twophone.h    shared/c/test, the kernel test harness; test-only,
 *                          nothing that ships includes them, but a test is
 *                          compiled by a build system like any other file, so
 *                          each product's test header spells them relatively
 *   text_util.h            shared/c/text_util, the UTF-8 stepping, column
 *                          counting and buffer writers under four products'
 *                          say layers; in its own directory beside its test so
 *                          the builds that wildcard shared/c/*.c pick up neither
 *   wire_check.h           shared/c/wire_check, the envelope's truncated SHA-256
 *                          over head and body under four products' message
 *                          coders; in its own directory for the same reason
 *   stats.h, seed_hash.h   shared/c/stats, the mean / standard error / Wilson
 *                          interval and the per-game seed hash under the bot
 *                          arenas and the test helpers that seed games; dev
 *                          tools only, so no kernel build list names stats.c
 *                          and the per-build check below does not list it
 *   msg_seat_tag.h         shared/c/msg_seat_tag, the roster row, the nickname
 *                          verdict and the record / tag / sender / name seat
 *                          resolver under three products' message coders; in
 *                          its own directory for the wildcard reason too
 *   msg_lobby_roster.h     shared/c/msg_lobby_roster, the lobby's rules (cap,
 *                          join, leave, the one control offered, the start
 *                          through the product's game constructor, the
 *                          roster's changes) under three products' lobby
 *                          adapters; in its own directory for the wildcard
 *                          reason too
 *   collapse.h             shared/c/collapse, the auto-collapse's push (the
 *                          host's drawer spring over the slide) and its two
 *                          numbers; header-only (static inline), so no build
 *                          list names a .c for it and the per-build check
 *                          below does not list it; Swift may reach it through
 *                          its module.modulemap
 *   le_bytes.h             shared/c/le_bytes.h, the fixed-width little-endian
 *                          u16 / u32 / u48 put and get under the card kernel's
 *                          blobs, wires and session log; header-only (static
 *                          inline), so no build list names a .c for it either
 */
const SHARED_HEADERS = ['sha256.h', 'deal_rng.h', 'b32.h', 'mixrad.h', 'languages.h', 'msg_stage.h', 'motion_ruler.h',
    'check.h', 'twophone.h', 'text_util.h', 'wire_check.h', 'stats.h', 'seed_hash.h',
    'msg_seat_tag.h', 'msg_lobby_roster.h', 'collapse.h', 'le_bytes.h'];

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
    const expectedProbes = [
        'shared/c/sha256.c',
        'shared/c/deal_rng.c',
        'shared/c/b32.c',
        'shared/c/mixrad.c',
        'shared/c/text_util/text_util.c',
        'shared/c/wire_check/wire_check.c',
        'shared/c/stats/stats.c',
        'shared/c/stats/stats_test.c',  // stats.h, seed_hash.h
        'shared/c/msg_seat_tag/msg_seat_tag.c',
        'shared/c/msg_lobby_roster/msg_lobby_roster.c',
        'foolish/c/src/game.c',
        'foolish/c/src/main_eval.c',    // stats/stats.h
        'werewolf/c/src/ww_game.c',
        // One TU per newer product, chosen for the shared headers it includes.
        'uttt/c/src/uttt_code.c',       // b32.h, mixrad.h
        'uttt/c/src/uttt_lang.c',       // i18n/languages.h
        'uttt/c/src/uttt_msg.c',        // sha256.h, b32.h, wire_check.h
        'pickemup/c/src/pk_deck.c',     // deal_rng.h
        'pickemup/c/src/pk_msg.c',      // sha256.h, b32.h, wire_check.h, msg_seat_tag.h (via pk_msg.h)
        'chuiniu/c/src/cn_dice.c',      // deal_rng.h, sha256.h
        'chuiniu/c/src/cn_code.c',      // mixrad.h
        'chuiniu/c/src/cn_msg.c',       // sha256.h, b32.h, wire_check.h, msg_seat_tag.h (via cn_msg.h)
        'tallybones/c/src/tb.c',        // sha256.h, deal_rng.h
        'tallybones/c/src/tb_msg.c',    // sha256.h, b32.h, wire_check.h, msg_seat_tag.h (via tb_msg.h)
        // The bridges, which reach the seat module through the message header.
        'pickemup/c/ios/pk_api.c',      // msg_seat_tag.h (via ../src/pk_msg.h)
        'chuiniu/c/ios/cn_api.c',       // msg_seat_tag.h (via ../src/cn_msg.h)
        'tallybones/c/ios/tb_api.c',    // msg_seat_tag.h (via ../src/tb_msg.h)
        // The lobby adapters, which include the shared lobby rules directly.
        'pickemup/c/src/pk_lobby.c',    // msg_lobby_roster.h (via pk_lobby.h)
        'chuiniu/c/src/cn_lobby.c',     // msg_lobby_roster.h (via cn_lobby.h)
        'tallybones/c/src/tb_lobby.c',  // msg_lobby_roster.h (via tb_lobby.h)
        'tallybones/c/src/tb_code.c',   // mixrad.h
        'uttt/c/src/uttt_say.c',        // text_util.h
        'pickemup/c/src/pk_say.c',      // text_util.h
        'chuiniu/c/src/cn_say.c',       // text_util.h
        'tallybones/c/src/tb_say.c',    // text_util.h
        // The test harness, reached only from tests.
        'chuiniu/c/tests/cn_test.c',            // test/check.h, through cn_check.h
        'tallybones/c/tests/tb_twophone_test.c', // test/twophone.h
        // The arenas and the seeding test helpers, reached only from dev tools.
        'pickemup/c/tools/pk_arena.c',          // stats/seed_hash.h
        'chuiniu/c/bot/cn_arena.c',             // stats/stats.h, stats/seed_hash.h
        'tallybones/c/bot/tb_solve.c',          // stats/stats.h
        'tallybones/c/tests/tb_test.c',         // stats/seed_hash.h, through tb_check.h
        // The auto-collapse push, header-only: its test, and the two files that
        // forward to it.
        'shared/c/collapse/collapse_test.c',
        'uttt/c/src/uttt_anim.c',       // collapse/collapse.h (via uttt_anim.h)
        'pickemup/c/ios/pk_lay.c',      // collapse/collapse.h
        // The little-endian integers, header-only: its test, and the kernel
        // file that writes the blob's clocks through it.
        'shared/c/le_bytes_test.c',
        'foolish/c/src/view.c',         // le_bytes.h
    ];
    const probes = expectedProbes.filter((p) => existsSync(join(REPO, p)));
    assert.equal(probes.length, expectedProbes.length,
        'a probe source is missing - did the kernel move again?\n  '
        + expectedProbes.filter((p) => !probes.includes(p)).join('\n  '));

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
            // The name must start at a quote, a slash or a space: check.h is the
            // tail of wire_check.h, and a bare substring test names both.
            for (const h of SHARED_HEADERS) {
                const at = (tail: string) => new RegExp(`(^|['"/\\s])${h.replace(/\./g, '\\.')}${tail}`, 'm');
                if (at("' file not found").test(stderr) || at(': No such file').test(stderr)) {
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
    // LINK the shared sources; a list that stopped naming one fails at the
    // link, far from the cause. Asserted by asking each Makefile what it thinks
    // its source list is, rather than by reading the list here.
    //
    // The expected set is PER BUILD, and it is what that build's own sources
    // include today: uttt deals no cards, so it has no deal_rng.c; the card
    // kernel and the third product code nothing in base32 or mixed radix. (The
    // sim and the native server take $(wildcard shared/c/*.c), so they compile
    // b32.c and mixrad.c too; what they must not lose is the pair they link.)
    // wire_check.c follows the message coders: the replay page's WEB_WASM_SRC
    // has no message coder in it, so it has no wire check either.
    // msg_seat_tag.c is the tag-and-record seat identity of the three newer
    // message coders; uttt keeps its own three-witness resolver, and the card
    // kernel and the third product resolve seats a different way (name and
    // cached seat, no tag).
    // msg_lobby_roster.c is the roster-struct lobby of the same three; uttt's
    // lobby is two fixed seats (the joiner sits by making the first move), and
    // the card kernel and the third product decide their lobbies over scalar
    // arguments with no roster struct.
    const KERNEL = ['deal_rng.c', 'sha256.c'];
    const NEWER = ['deal_rng.c', 'sha256.c', 'b32.c', 'mixrad.c', 'text_util.c', 'wire_check.c', 'msg_seat_tag.c',
        'msg_lobby_roster.c'];
    const builds: Array<{ make: string; variable: string; shared: string[] }> = [
        { make: 'foolish/c', variable: 'CORE_SRC', shared: KERNEL },
        { make: 'foolish/foolyard', variable: 'KERNEL_SRC', shared: KERNEL },
        { make: 'foolish/server/impls/native', variable: 'KERNEL_SRC', shared: KERNEL },
        { make: 'werewolf/c', variable: 'CORE_SRC', shared: KERNEL },
        { make: 'uttt/c', variable: 'SRC', shared: ['sha256.c', 'b32.c', 'mixrad.c', 'text_util.c', 'wire_check.c'] },
        { make: 'uttt/c', variable: 'WEB_WASM_SRC', shared: ['b32.c', 'mixrad.c', 'text_util.c'] },
        { make: 'pickemup/c', variable: 'SRC', shared: NEWER },
        { make: 'chuiniu/c', variable: 'SRC', shared: NEWER },
        { make: 'tallybones/c', variable: 'SRC', shared: NEWER },
    ];
    const missing: string[] = [];
    for (const { make, variable, shared } of builds) {
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
        for (const base of shared) {
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
    // five lines of English in it every time CI did. The hash skips a path that
    // is not a file, so junk in this list is a source the hash has quietly
    // stopped covering - and that hash is what decides whether the uncommitted
    // test module gets rebuilt (e2e/helpers/bots_test_wasm.ts).
    //
    // Running it BOTH ways and demanding the same answer is the cheapest way to
    // stop that being a CI-only discovery.
    const run = (env: NodeJS.ProcessEnv) =>
        execFileSync('bash', [join(PRODUCT, 'scripts/wasm_stamp.sh'), '--list'],
            { cwd: PRODUCT, encoding: 'utf8', env }).trim().split('\n');

    const plain = run({ ...process.env, MAKELEVEL: undefined, MAKEFLAGS: undefined });
    const underMake = run({ ...process.env, MAKELEVEL: '1', MAKEFLAGS: 'w' });

    assert.ok(plain.length > 50, `the source list came back with only ${plain.length} entries`);
    assert.deepEqual(underMake, plain,
        'scripts/wasm_stamp.sh lists different sources depending on whether make is\n'
        + 'its caller. That is "make[1]: Entering directory" landing in the list -\n'
        + 'add --no-print-directory to the make call that grew it.');

    const missing = plain.filter((p) => !existsSync(join(PRODUCT, p)));
    assert.deepEqual(missing, [], `these listed wasm sources do not exist:\n  ${missing.join('\n  ')}`);
});
