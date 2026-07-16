/* =============================================================================
 * The kernel's one-tap cover resolver is legal and unambiguous (A7/F9)
 * =============================================================================
 * This began as the cutover safety net for moving coverCombinations.ts
 * findUnambiguousCover into the kernel (legal.c unambiguous_cover): it fuzzed
 * random cover selections and insisted the kernel made the SAME decision the TS
 * resolver made. That TS is now deleted, so there is no second implementation
 * left to agree with — and agreeing with a copy was never the real property
 * anyway (docs/C_CORE_CONSOLIDATION.md A9). What matters, and what this now
 * asserts over a fuzz the hand-picked C cases in tests.c cannot cover, is that
 * EVERY cover the kernel accepts is genuinely legal and unambiguous:
 *   - each cover card really covers the attack it is paired with (can_cover),
 *   - every covered attack was actually uncovered on the table, none twice, and
 *   - the same cards can never cover two DIFFERENT sets of attacks (if they
 *     could, the kernel had to refuse — that is the whole point of the resolver).
 * The mirror-vs-kernel comparison lived in the commit that made the cutover.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ensureBotsAsync, kernelUnambiguousCover } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { canCover } from '../supabase/functions/_shared/common_utils.ts';
import { Card, Battle } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Deterministic LCG so a failure reproduces.
let seed = 0x1234abcd >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

const card = (suit: number, value: number): Card => ({ suit, value });
const key = (c: Card) => `${c.suit}-${c.value}`;

// A random small table + cover selection, dealt from DISTINCT cards — a real
// deck holds one of each, and duplicate-card tables are not a state the game can
// reach (the resolver's behaviour on them is undefined and not worth pinning).
// Drawn from a deliberately narrow rank band (6..11) so cards share ranks/suits
// often and genuine ambiguities (one card able to cover two attacks) come up.
function randomCase(): { cover: Card[]; battles: Battle[]; trump: number } {
    const trump = ri(4);
    // Build a small pool of distinct cards and deal without replacement.
    const pool: Card[] = [];
    for (let s = 0; s < 4; s++) for (let v = 6; v <= 11; v++) pool.push(card(s, v));
    for (let i = pool.length - 1; i > 0; i--) { const j = ri(i + 1); [pool[i], pool[j]] = [pool[j], pool[i]]; }
    let p = 0;
    const draw = () => pool[p++];

    const nBattles = 1 + ri(3);
    const battles: Battle[] = [];
    for (let i = 0; i < nBattles && p < pool.length - 1; i++) {
        const attack = draw();
        // ~40% already covered — the resolver must ignore those; only uncovered
        // attacks are in play. The defense is a distinct drawn card too.
        const covered = rnd() < 0.4;
        battles.push({ attack, defense: covered ? draw() : null });
    }
    const nCover = 1 + ri(3);
    const cover: Card[] = [];
    for (let i = 0; i < nCover && p < pool.length; i++) cover.push(draw());
    return { cover, battles, trump };
}

// Independent, test-local oracle: every DISTINCT set of attacks that some valid
// full pairing of the cover cards could cover. Simple brute force — the inputs
// are tiny — and deliberately NOT the production resolver, so it is a real
// second opinion rather than the thing under test. The cover is unambiguous iff
// this yields exactly one set.
function achievableCoveredSets(cover: Card[], uncovered: Card[], trump: number): Set<string> {
    const sets = new Set<string>();
    const used: boolean[] = new Array(uncovered.length).fill(false);
    const chosen: number[] = [];
    const recurse = (depth: number) => {
        if (depth === cover.length) {
            sets.add([...chosen].sort((a, b) => a - b).join(','));
            return;
        }
        for (let j = 0; j < uncovered.length; j++) {
            if (used[j] || !canCover(uncovered[j], cover[depth], trump)) continue;
            used[j] = true; chosen.push(j);
            recurse(depth + 1);
            chosen.pop(); used[j] = false;
        }
    };
    if (cover.length > 0 && cover.length <= uncovered.length) recurse(0);
    return sets;
}

test('every cover the kernel accepts is legal and unambiguous', async () => {
    await ensureBotsAsync();

    let accepts = 0, refusals = 0;
    const CASES = 4000;
    for (let i = 0; i < CASES; i++) {
        const { cover, battles, trump } = randomCase();
        const uncovered = battles.filter((b) => !b.defense).map((b) => b.attack);
        const k = kernelUnambiguousCover(cover, battles, trump);

        // The independent oracle's verdict: unambiguous iff exactly one covered set.
        const setsCount = achievableCoveredSets(cover, uncovered, trump).size;
        const shouldAccept = setsCount === 1;

        assert.equal(k !== null, shouldAccept,
            `case ${i}: kernel ${k ? 'accepted' : 'refused'} but there are ${setsCount} achievable covered sets\n`
            + `  cover=${JSON.stringify(cover)} battles=${JSON.stringify(battles)} trump=${trump}`);

        if (k === null) { refusals++; continue; }
        accepts++;

        // The kernel's own pairing must be legal: card j covers attack j, every
        // covered attack was uncovered on the table, and none is covered twice.
        assert.equal(k.coverCards.length, k.attackCards.length, `case ${i}: pairing length`);
        const uncoveredKeys = new Set(uncovered.map(key));
        const usedAttacks = new Set<string>();
        for (let j = 0; j < k.coverCards.length; j++) {
            assert.ok(canCover(k.attackCards[j], k.coverCards[j], trump),
                `case ${i}: cover card ${j} does not actually cover its attack`);
            assert.ok(uncoveredKeys.has(key(k.attackCards[j])),
                `case ${i}: covered an attack that was not uncovered on the table`);
            assert.ok(!usedAttacks.has(key(k.attackCards[j])),
                `case ${i}: covered the same attack twice`);
            usedAttacks.add(key(k.attackCards[j]));
        }
        assert.ok(k.coverCards.length === cover.length,
            `case ${i}: a full cover must use every selected card`);
    }

    // The fuzz must actually exercise both outcomes, or it proves nothing.
    assert.ok(accepts > 100, `only ${accepts} covers were accepted — fuzz too weak`);
    assert.ok(refusals > 100, `only ${refusals} covers were refused — fuzz too weak`);
    console.log(`kernel cover resolver: ${accepts} accepted, ${refusals} refused across ${CASES} cases`);
});
