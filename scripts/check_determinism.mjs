#!/usr/bin/env node
/* =============================================================================
 * The determinism gate: one draw of real entropy in the whole system
 * =============================================================================
 * The invariant, in the owner's words: the only true nondeterministic
 * randomness should be when we seed a live game. That is ONE crypto draw, at
 * the deal (injectDealSeed, sdk/ts/wasm/engine.ts). Mid-game engine randomness
 * and bot decisions are both reseeded from that deal seed, so a whole game
 * replays from it, and a test that pins the seed pins the game.
 *
 * It has been broken before. The comments in engine.ts and bots.ts record a
 * per-move Math.random reseed that had to be removed, and nothing enforced the
 * rule afterwards. This does.
 *
 * FOUR RULES, each with its own scope, because "nondeterministic" means
 * different things in a server and in a test:
 *
 *   entropy rules - Math.random, randomUUID, getRandomValues/randomBytes.
 *     Scope: every file in e2e/, sdk/ and server/. A draw anywhere on the game
 *     path breaks replay; a draw in a test makes every run a different
 *     experiment and hands a red CI log no repro line.
 *
 *   clock rule - Date.now(), new Date() with no argument, performance.now().
 *     Scope: e2e test FILES only (e2e/**\/*.test.ts). A server reads the clock
 *     constantly and must - expiry, leases, [TIMING] logs - and none of that
 *     decides a card. What must not happen is a test whose VERDICT depends on
 *     the machine's clock. Benches and harnesses under e2e/ are out of scope
 *     for the same reason: measuring elapsed time is their job.
 *
 * It matches CALLS, not the string, so the comments that record the removed
 * reseeds do not trip it. `Math.random = <seeded lcg>` (the bot_parity pattern:
 * pinning code you do not own) is an assignment, not a draw, and passes too.
 *
 * src/ is deliberately out (cosmetic textures, React keys, error ids) and
 * offlinefun/ is out (research arenas, where randomness is the point).
 *
 * Allowlisting is per (RULE, FILE) with an expected call count and a reason,
 * never per directory: adding a draw to an allowed file still fails until
 * someone edits this list, so every exception is an arguable line in a diff.
 * An allowlist entry whose calls have since gone away also fails, so the list
 * cannot rot into a blanket permission.
 *
 * e2e/determinism_gate.test.ts proves this file can fail, by running scan()
 * over fixtures that break each rule. A gate nobody has seen go red is not a
 * gate.
 * ========================================================================== */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOTS = ['e2e', 'sdk', 'server'];
const EXTS = ['.ts', '.tsx', '.mts', '.cts', '.js', '.mjs', '.cjs', '.jsx'];
const SKIP_DIRS = new Set(['node_modules', '.git', 'dist', 'build', '.next']);

// A call, not a mention. The name must be followed by "(" and may carry a
// receiver chain, so crypto.randomUUID() and globalThis.crypto.randomUUID()
// both match. The receiver part is why this is not the older
// `(^|[^.\w$])NAME\(` shape: that one rejects anything with a "." in front of
// the name, which is fine for a bare global like Math but blind to every
// method hanging off crypto.
const call = (name) => new RegExp(`(^|[^\\w$])(?:[A-Za-z_$][\\w$]*\\s*\\.\\s*)*${name}\\s*\\(`, 'g');

const EVERYWHERE = () => true;
const E2E_TESTS = (rel) => rel.startsWith('e2e/') && rel.endsWith('.test.ts');

export const RULES = [
    {
        id: 'math-random',
        what: 'Math.random()',
        scope: EVERYWHERE,
        patterns: [call('Math\\s*\\.\\s*random')],
        use: 'tests: e2e/helpers/rng.ts suiteRng(<suite>). engine: the deal seed. '
            + 'bots: the deal-seed-derived stream (rngBaseFromSeed), never a fresh draw.',
    },
    {
        id: 'random-uuid',
        what: 'crypto.randomUUID()',
        scope: EVERYWHERE,
        patterns: [call('randomUUID')],
        use: 'derivedUuid(namespace, seq) from sdk/ts/wire/detid.ts - a UUID shape that is '
            + 'a function of its inputs. Real entropy only for something unguessable '
            + '(a join code, a lease token), and that needs an allowlist entry saying so.',
    },
    {
        id: 'random-bytes',
        what: 'crypto.getRandomValues() / randomBytes()',
        scope: EVERYWHERE,
        patterns: [call('getRandomValues'), call('randomBytes')],
        use: 'the deal seed is the one place that draws bytes. Tests pin it with '
            + '__setDealSeedOverride.',
    },
    {
        id: 'clock',
        what: "a clock read (Date.now(), new Date(), performance.now())",
        scope: E2E_TESTS,
        patterns: [
            call('Date\\s*\\.\\s*now'),
            call('performance\\s*\\.\\s*now'),
            /(^|[^.\w$])new\s+Date\s*\(\s*\)/g,
        ],
        use: 'pin the clock instead: __setEngineClock(() => <fixed ms>) for the engine, '
            + 'the `now:` / `ctx.now` parameters the wire and view builders already take, '
            + 'or just a constant. `new Date(<ms>)` with an argument is fine - it is a '
            + 'conversion, not a reading.',
    },
];

export const ALLOW = [
    {
        rule: 'clock',
        file: 'e2e/oracle_mode_b.test.ts',
        calls: 2,
        // Not a verdict, a BUDGET. Both Oracle modes are wall-clock bounded by
        // construction - the panel deliberates until its time is up - so a test
        // that compares them has to bound its own loops the same way. These two
        // reads set and check that budget (`end = now + msCap`, then the loop
        // condition); no assertion reads a clock, and every verdict in the file
        // comes from folded integer sums that thread interleaving cannot move.
        // Removing them would not make the test deterministic, it would make it
        // unbounded.
        reason:
            'A wall-clock BUDGET for a benchmark loop, not a verdict: both Oracle '
            + 'modes are wall-clock bounded by construction, so the comparison has to '
            + 'bound itself the same way. No assertion in the file reads a clock.',
    },
    {
        rule: 'math-random',
        file: 'server/impls/supabase/functions/_shared/adapter/meta_actions.ts',
        calls: 1,
        // TEMPORARY, not a blessing. This is the only reachable Math.random() in
        // server/. It picks which bot joins a LOBBY when the client sends no bot
        // id - and the one client that does that is the iOS app, whose lobby is a
        // plain "Add Bot" button. The web has a picker and passes an id. Two
        // clients answering "which bot gets added" differently is the actual
        // problem; this draw is the symptom.
        // Deleting the branch now would leave iOS "Add Bot" unable to add
        // anything, so it stays until the iOS picker lands, and this entry goes
        // with it. See "Queued: the iOS lobby needs the bot picker" in
        // docs/KERNEL_LIFT_BRIEF.md for the three steps.
        reason:
            'TEMPORARY: the no-bot-id fallback the iOS lobby still relies on, having '
            + 'no bot picker. Delete with the server branch once the picker lands - '
            + 'see "Queued: the iOS lobby needs the bot picker" in docs/KERNEL_LIFT_BRIEF.md.',
    },
    {
        rule: 'random-bytes',
        file: 'sdk/ts/wasm/engine.ts',
        calls: 1,
        // THE draw. Everything in this file exists to protect this one line.
        reason:
            'THE one entropic draw in the system: the 32-byte deal seed, once per live '
            + 'game, in injectDealSeed. The seed is saved to games.game_seed, so the game '
            + 'replays from it; tests pin it with __setDealSeedOverride.',
    },
    {
        rule: 'random-uuid',
        file: 'server/api/common/common_utils.ts',
        calls: 1,
        // createId(). Note the caveat - it is worth someone's attention, but it
        // is a width problem, not a determinism problem.
        reason:
            'createId(): the game id, which is also the code a player shares to join, so '
            + 'it must be unguessable. Part of "seeding a live game", not a defect. '
            + 'CAVEAT: it is randomUUID().slice(0, 6) - 24 bits, which collides at a few '
            + 'thousand live games. Widen it; do not derive it.',
    },
    {
        rule: 'random-uuid',
        file: 'server/impls/supabase/functions/_shared/adapter/utils.ts',
        calls: 2,
        reason:
            'Two correlation tokens on the LIVE server, neither of which any reader '
            + 'compares or orders by: the broadcast envelope sequence id (the client '
            + 'dedupes an animation sequence by it; playback order comes from the version) '
            + 'and a request-id prefix for log lines. Nothing replays these.',
    },
    {
        rule: 'clock',
        file: 'e2e/auth_jwt.test.ts',
        calls: 1,
        reason:
            'The subject under test is JWT expiry, and verifyJwtLocal reads the real '
            + 'clock inside. Minting exp/iat relative to the same clock is the test, not '
            + 'a leak: pinning only this side would assert against a token the verifier '
            + 'considers ancient.',
    },
    {
        rule: 'clock',
        file: 'e2e/wasm_kernel_fuzz.test.ts',
        calls: 4,
        reason:
            'Two hang guards over malformed kernel input (2s and 3s ceilings on work that '
            + 'takes milliseconds). The clock is what "did not hang" means. The margin is '
            + 'three orders of magnitude, so machine speed cannot flip the verdict.',
    },
    {
        rule: 'clock',
        file: 'e2e/client_guards.test.ts',
        calls: 2,
        reason:
            'A µs/call number printed to stderr beside the gate-call loop. Nothing asserts '
            + 'on it - the assertions in that test are about the guard answers and the '
            + 'wasm memory high-water mark.',
    },
];

/* Strip comments and string bodies so a call is a call, keeping line numbers.
 * Quote-aware on purpose: a "//" inside a string must not blank the rest of the
 * line, or a real draw after it would go unseen. */
export function codeOnly(src) {
    let out = '';
    let i = 0;
    const n = src.length;
    while (i < n) {
        const c = src[i];
        const d = src[i + 1];
        if (c === '/' && d === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }
        if (c === '/' && d === '*') {
            i += 2;
            while (i < n && !(src[i] === '*' && src[i + 1] === '/')) { if (src[i] === '\n') out += '\n'; i++; }
            i += 2;
            continue;
        }
        if (c === '"' || c === "'" || c === '`') {
            i++;
            while (i < n && src[i] !== c) {
                if (src[i] === '\\') i++;
                else if (src[i] === '\n') out += '\n';
                i++;
            }
            i++;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

function* walk(dir) {
    for (const name of readdirSync(dir).sort()) {
        if (SKIP_DIRS.has(name)) continue;
        const p = join(dir, name);
        const st = statSync(p);
        if (st.isDirectory()) yield* walk(p);
        else if (EXTS.some((e) => name.endsWith(e))) yield p;
    }
}

/**
 * Run every rule over `roots` under `root`. Returns the problems as strings -
 * empty means the gate passes. Exported so e2e/determinism_gate.test.ts can
 * point it at fixtures and watch it go red.
 */
export function scan({ root, roots = ROOTS, rules = RULES, allow = ALLOW } = {}) {
    // (rule id -> (relative path -> line numbers))
    const hits = new Map(rules.map((r) => [r.id, new Map()]));

    for (const dirName of roots) {
        const abs = join(root, dirName);
        try { statSync(abs); } catch { continue; }
        for (const file of walk(abs)) {
            const rel = relative(root, file).split(sep).join('/');
            const applicable = rules.filter((r) => r.scope(rel));
            if (!applicable.length) continue;
            const lines = codeOnly(readFileSync(file, 'utf8')).split('\n');
            for (const rule of applicable) {
                const found = [];
                lines.forEach((line, idx) => {
                    for (const re of rule.patterns) for (const _ of line.matchAll(re)) found.push(idx + 1);
                });
                if (found.length) hits.get(rule.id).set(rel, found.sort((a, b) => a - b));
            }
        }
    }

    const problems = [];
    for (const rule of rules) {
        for (const [file, lines] of hits.get(rule.id)) {
            const allowed = allow.find((a) => a.rule === rule.id && a.file === file);
            if (!allowed) {
                problems.push(
                    `${file}: ${lines.length} ${rule.what} at line(s) ${lines.join(', ')}\n`
                    + `      use ${rule.use}`,
                );
                continue;
            }
            if (lines.length !== allowed.calls) {
                problems.push(
                    `${file}: allowlisted for ${allowed.calls} ${rule.what}, found ${lines.length} `
                    + `at line(s) ${lines.join(', ')}.\n`
                    + `      the allowed one: ${allowed.reason}\n`
                    + '      A new one needs its own reason in scripts/check_determinism.mjs, not a bumped number.',
                );
            }
        }
    }
    for (const a of allow) {
        const rule = rules.find((r) => r.id === a.rule);
        if (!rule) { problems.push(`allowlist names an unknown rule "${a.rule}" - fix the entry.`); continue; }
        if (!hits.get(a.rule).has(a.file)) {
            problems.push(`${a.file}: allowlisted for ${rule.what} but has none left - drop the entry.`);
        }
    }
    return problems;
}

/** The count each rule matched, for a one-line summary. */
export function ruleSummary(problems) {
    return problems.length === 0;
}

const ROOT = join(fileURLToPath(new URL('.', import.meta.url)), '..');

// Only when run as the CLI, so importing scan() from a test does not exit.
if (process.argv[1] && fileURLToPath(new URL(`file://${process.argv[1]}`)).endsWith('check_determinism.mjs')) {
    const problems = scan({ root: ROOT });
    if (problems.length) {
        process.stderr.write(
            'determinism gate failed.\n\n'
            + 'The invariant: the only true nondeterministic draw in the system is the one\n'
            + 'that seeds a live game - 32 crypto bytes at the deal (injectDealSeed in\n'
            + 'sdk/ts/wasm/engine.ts). Mid-game engine randomness and bot decisions are both\n'
            + 'reseeded from that deal seed, so a whole game replays from it. Entropy in\n'
            + 'e2e/, sdk/ or server/ breaks that, and a clock read that decides a test\n'
            + 'verdict breaks it from the outside.\n\n'
            + `${problems.map((p) => `  - ${p}`).join('\n')}\n\n`
            + 'If a hit is genuinely one of the few things that must stay entropic - a join\n'
            + 'code, a lease token, the deal seed - add it to ALLOW in this file with the\n'
            + 'reason beside it. The reason is the point; the entry is the diff someone\n'
            + 'gets to argue with.\n',
        );
        process.exit(1);
    }
    process.stdout.write(
        `determinism gate: ok (${ROOTS.join(', ')}; ${RULES.length} rules, `
        + `${ALLOW.reduce((n, a) => n + a.calls, 0)} allowlisted call sites)\n`,
    );
}
