// A shipped wasm module is a BUILD OUTPUT. It is not committed, and this is the
// gate that keeps it that way.
//
// This is the same gate as generated_outputs_validation.test.ts, one step
// further down the same argument, and the repo has now paid for the committed
// arrangement twice.
//
//   verify.wasm sat committed under tools/structgen/gen/ with
//   `diff -x verify.wasm` excusing it from the freshness check, and the
//   exclusion is what let it rot for months (f6338351). That is why nothing
//   under sdk/ts/gen is committed.
//
//   public/oracle.wasm.gz then did it again, in the open: it sat unrebuilt from
//   2026-08-23 missing podkidnoy and the solver seat fix, so the live Infinite
//   Oracle served one of two seats the opposite endgame verdict. It shipped and
//   nothing noticed. The answer at the time was scripts/check_wasm_freshness.sh,
//   which could not compare bytes and so compared COMMIT ORDER, plus
//   sdk/ts/wasm/WASM_STAMP, a source hash committed beside the artifacts to
//   prove a human had run make. Two gates, one cause. On the day they were
//   deleted the freshness report still said oracle.wasm.gz and
//   oracle-mt.wasm.gz were BEHIND the kernel, and the drift was exactly ONE raw
//   byte - which is not something review finds.
//
// So the modules are built by the lanes that consume them (scripts/wasm_build.sh),
// and re-committing one must be impossible to do by accident. Three facts,
// checked here, deliberately mirroring the structgen gate:
//
//   1. git tracks NO shipped wasm artifact.
//   2. each one is ignored, so a freshly built tree is clean and a `git add -A`
//      cannot sweep one back in.
//   3. the retired gates stay retired - a committed artifact cannot come back
//      by re-adding the scaffolding that used to hold one up.
//
// The paths come from `wasm_build.sh --print-paths` rather than from a list kept
// here: the builder is asked what it builds. That flag needs no toolchain, which
// is why this gate lives in the fast lane and runs on every pull request while
// the compiler-shaped checks stay in wasm.yml.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const BUILD_SH = 'scripts/wasm_build.sh';

const run = (cmd: string, args: string[]) =>
    execFileSync(cmd, args, { cwd: REPO, encoding: 'utf8', env: { ...process.env, GIT_OPTIONAL_LOCKS: '0' } });

const git = (...args: string[]) => run('git', args);

/** What wasm_build.sh says it writes, repo-relative. */
function builtArtifacts(): string[] {
    return run('bash', [BUILD_SH, '--print-paths']).split('\n').map((s) => s.trim()).filter(Boolean);
}

test('wasm_build.sh answers for its own outputs', () => {
    const paths = builtArtifacts();
    // Three today: the kernel every host loads, and the Oracle's two modules.
    // The count is asserted so a --print-paths that starts answering with
    // nothing fails HERE rather than making the checks below pass over an empty
    // list - the exact failure mode `diff -x verify.wasm` had.
    assert.ok(paths.length >= 3,
        `${BUILD_SH} --print-paths answered with ${paths.length} paths: ${paths.join(', ')}`);
    for (const p of paths) {
        assert.ok(!p.startsWith('/') && !p.includes('..'),
            `${p} is not a repo-relative path - this gate resolves these against the repo root`);
    }
    // Every path belongs to a group, so `wasm_build.sh <group>` can build a
    // subset and no artifact is reachable only by building all of them.
    const groups = run('bash', [BUILD_SH, '--print-groups']).split('\n').map((s) => s.trim()).filter(Boolean);
    assert.ok(groups.length >= 1, `${BUILD_SH} --print-groups answered with nothing`);
    const grouped = new Set(groups.flatMap((g) =>
        run('bash', [BUILD_SH, '--print-paths', g]).split('\n').map((s) => s.trim()).filter(Boolean)));
    assert.deepEqual([...paths].filter((p) => !grouped.has(p)), [],
        'these artifacts are in no group, so a lane cannot ask for them by name');
});

test('no shipped wasm artifact is tracked in the repo', () => {
    const paths = builtArtifacts();
    const tracked = git('ls-files', '--', ...paths).split('\n').filter(Boolean);
    assert.deepEqual(tracked, [],
        'these are build outputs and must not be committed. The lanes that consume\n'
        + 'them build them (scripts/wasm_build.sh), so a committed copy is a copy\n'
        + 'nothing keeps fresh - which is how public/oracle.wasm.gz shipped one seat\n'
        + 'the opposite endgame verdict for three weeks. Remove them with:\n'
        + `  git rm --cached ${tracked.join(' ')}\n`);
});

test('every shipped wasm artifact is ignored', () => {
    const notIgnored: string[] = [];
    for (const p of builtArtifacts()) {
        // --no-index, and the path need not exist: check-ignore answers about
        // the path, so this holds on a fresh checkout nothing has built into.
        try {
            git('check-ignore', '-q', '--no-index', '--', p);
        } catch {
            notIgnored.push(p);
        }
    }
    assert.deepEqual(notIgnored, [],
        'these are written by every lane that consumes them but are not ignored, so a\n'
        + 'freshly built tree shows up as modified and one `git add -A` puts them back:\n'
        + `  ${notIgnored.join('\n  ')}\n`);
});

test('the gates that propped up a committed artifact stay retired', () => {
    // Not nostalgia - a SHRINK-ONLY list, the same shape as the
    // NOT_YET_GENERATED allowlist in e2e/no_ts_game_shape.test.ts. Each of these
    // existed only because the artifact was committed and a human built it by
    // hand. Bringing one back means the artifact came back too, and that is the
    // review this test exists to force.
    //
    // WASM_STAMP is the subtle one and the reason this test names files rather
    // than trusting the two above. It was never a compiled artifact: it is a
    // sha256 over the wasm SOURCE SET, so it is identical from every toolchain
    // (measured e3fa5e8b... from all three of clang 18/Linux, clang 22/Linux and
    // clang 22/macOS). It said "someone ran make", which is a fact a lane that
    // runs make itself does not need - so it would sail past a gate that only
    // asks "is this file a build output".
    //
    // scripts/wasm_stamp.sh itself SURVIVES and must not be added here: --list
    // and --hash are the repo's one derivation of what a wasm source is, and
    // e2e/helpers/bots_test_wasm.ts and c/Makefile both read it. Only --write
    // and the file it wrote are gone.
    const RETIRED = [
        'sdk/ts/wasm/WASM_STAMP',
        'scripts/check_wasm_freshness.sh',
    ];
    const back = git('ls-files', '--', ...RETIRED).split('\n').filter(Boolean);
    assert.deepEqual(back, [],
        'these were the scaffolding under a COMMITTED wasm artifact, and they are\n'
        + 'retired because the artifact is not committed any more:\n'
        + `  ${back.join('\n  ')}\n`
        + 'A committed artifact needs a gate to say whether it is stale; a built one\n'
        + 'cannot be stale. If one of these is genuinely needed again, the thing to\n'
        + 'argue about first is why an artifact is back in git.\n');
});
