#!/usr/bin/env node
/* =============================================================================
 * The determinism gate: no Math.random() in e2e/, sdk/ or server/
 * =============================================================================
 * The invariant: ONE crypto draw per game, at the deal (injectDealSeed,
 * sdk/ts/wasm/engine.ts). Everything after it derives - mid-game engine
 * randomness and bot decisions are both reseeded from that deal seed, so a whole
 * game replays from it. A Math.random() anywhere in the game path breaks that,
 * and it has been broken before: the comments in engine.ts and bots.ts record a
 * per-move Math.random reseed that had to be removed. Nothing enforced the rule
 * afterwards. This does.
 *
 * It matches CALLS, not the string, so the comments that record the removed
 * reseeds do not trip it. `Math.random = <seeded lcg>` (the bot_parity pattern:
 * pinning code you do not own) is a patch, not a draw, and passes too.
 *
 * Scope is deliberate. src/ is out (cosmetic textures, React keys, error ids)
 * and offlinefun/ is out (research arenas, where randomness is the point).
 *
 * Allowlisting is per FILE with an expected call count and a reason, never per
 * directory: adding a draw to an allowed file still fails until someone edits
 * this list, so every exception is an arguable line in a diff.
 * ========================================================================== */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(fileURLToPath(new URL('.', import.meta.url)), '..');
const ROOTS = ['e2e', 'sdk', 'server'];
const EXTS = ['.ts', '.tsx', '.mts', '.cts', '.js', '.mjs', '.cjs', '.jsx'];
const SKIP_DIRS = new Set(['node_modules', '.git', 'dist', 'build', '.next']);

const ALLOW = [
    {
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
];

/* Strip comments and string bodies so a call is a call, keeping line numbers.
 * Quote-aware on purpose: a "//" inside a string must not blank the rest of the
 * line, or a real draw after it would go unseen. */
function codeOnly(src) {
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

const CALL = /(^|[^.\w$])Math\s*\.\s*random\s*\(/g;
const hits = new Map(); // relative path -> one entry per CALL, holding its line

for (const root of ROOTS) {
    const abs = join(ROOT, root);
    try { statSync(abs); } catch { continue; }
    for (const file of walk(abs)) {
        const lines = codeOnly(readFileSync(file, 'utf8')).split('\n');
        const found = [];
        lines.forEach((line, idx) => { for (const _ of line.matchAll(CALL)) found.push(idx + 1); });
        if (found.length) hits.set(relative(ROOT, file).split(sep).join('/'), found);
    }
}

const problems = [];
for (const [file, lines] of hits) {
    const allowed = ALLOW.find((a) => a.file === file);
    if (!allowed) { problems.push(`${file}: ${lines.length} Math.random() call(s) at line(s) ${lines.join(', ')}`); continue; }
    if (lines.length !== allowed.calls) {
        problems.push(
            `${file}: allowlisted for ${allowed.calls} Math.random() call(s), found ${lines.length} `
            + `at line(s) ${lines.join(', ')}. The allowed one: ${allowed.reason} `
            + 'A new draw needs its own reason in scripts/check_determinism.mjs, not a bumped number.',
        );
    }
}
for (const a of ALLOW) {
    if (!hits.has(a.file)) problems.push(`${a.file}: allowlisted but has no Math.random() call - drop the entry.`);
}

if (problems.length) {
    process.stderr.write(
        'determinism gate failed.\n\n'
        + 'The invariant: ONE crypto draw per game, at the deal (injectDealSeed in\n'
        + 'sdk/ts/wasm/engine.ts). Mid-game engine randomness and bot decisions are both\n'
        + 'reseeded from that deal seed, so a whole game replays from it. Math.random()\n'
        + 'in e2e/, sdk/ or server/ breaks that.\n\n'
        + 'Use instead:\n'
        + '  tests      e2e/helpers/rng.ts suiteRng(<suite>) - seeded from E2E_SEED_<SUITE>,\n'
        + '             prints its seed, and the seed belongs in the failure message.\n'
        + '  engine     the deal seed, via __setDealSeedOverride / __setKernelSeedSource.\n'
        + '  bots       the deal-seed-derived stream (rngBaseFromSeed), never a fresh draw.\n\n'
        + `${problems.map((p) => `  - ${p}`).join('\n')}\n`,
    );
    process.exit(1);
}

process.stdout.write(`determinism gate: ok (${ROOTS.join(', ')}, ${ALLOW.length} allowlisted call site)\n`);
