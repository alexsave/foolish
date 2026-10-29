// The bot roster is written down once, in the C registry, and the shipped
// kernel is the witness. This test makes the two agree.
//
// The canonical table is the kernel's (c/src/bots.c): one row per brain, and
// for a named bot its key, knobs, flags, tier and city, shared by the server,
// the phone and every future client (docs/C_CORE_CONSOLIDATION.md F1/A1). The
// server seats a bot by its key through that table on the C Table (table_load
// refuses a brain the build does not know), and derives its bot accounts from
// the seeded rows (foolish/server/src/store/bot_seed.c; foolish/server/test/
// unit_store.c holds them to production's 49).
//
// The SOURCE side is read through datagen (tools/structgen/gen.sh writes
// sdk/ts/gen/bot_registry.ts from bots.c with clang), not by parsing C as text,
// so a row written any way C accepts is read the way the compiler reads it. The
// kernel's own dump (kernelBotRoster) is then held to it, so the shipped module
// carries the table the source says.
import { test } from 'node:test';
import { kernelBotRoster } from '../sdk/ts/wasm/bots.ts';
import { BotRegistry as Rows } from '../sdk/ts/gen/bot_registry.ts';
import assert from 'node:assert/strict';

// The registry's rows as the C reads them: a field a row leaves out (a research
// brain has no key, an unseeded bot no city) is its zero value.
const BotRegistry = Rows.map((r) => ({
    name: r.name, key: r.key ?? '', knobs: r.knobs ?? '', uses_logs: r.uses_logs ?? 0, seeded: r.seeded ?? 0,
    offline: r.offline ?? 0, tier: r.tier ?? 0, city: r.city ?? '',
}));

/** The registry's named bots (rows with a key), in tier order: the roster. */
function sourceRoster() {
    const named = BotRegistry.filter((r) => r.key);
    return [...named].sort((a, b) => a.tier - b.tier);
}

test('C roster: the registry is well-formed and is the strength ladder', () => {
    const roster = sourceRoster();
    assert.ok(roster.length >= 10, `read only ${roster.length} named bots from bots.c - the generated table drifted?`);

    const keys = roster.map(r => r.key);
    assert.equal(new Set(keys).size, keys.length, 'roster keys must be unique');

    const tiers = roster.map(r => r.tier);
    assert.deepEqual(tiers, roster.map((_, i) => i + 1), 'the tiers must be 1..n with no gap or repeat');

    // The rungs the player-facing ladder is made of (docs/IOS_BOT_NAMING.md §1).
    assert.deepEqual(roster.filter(r => r.offline).map(r => r.key), [
        'random', 'simple_heuristic', 'handwritten', 'espresso', 'robusta',
        'firecracker', 'gunpowder', 'blackpowder', 'cordite', 'octogen',
    ], 'the offline picker rungs must match the bot-naming ladder, in order');
});

test('C roster: handwritten/espresso are the PRODUCTION mirrors, not arena variants', () => {
    // The arena/rollout variants (STRAT_HANDWRITTEN, STRAT_ESPRESSO) drifted from
    // the production bots and stay frozen for cordite's rollout policy. A
    // player-facing rung pointing at one of them is the §3 bug: offline
    // "Handwritten" was not the site's Handwritten.
    const by = (k: string) => BotRegistry.find(r => r.key === k)!;
    assert.equal(by('handwritten').name, 'handwritten_prod');
    assert.equal(by('espresso').name, 'espresso_prod');
});

test('C roster: the _max tiers are retired', () => {
    // octogen_max aliased octogen; cordite_max's flat CD_BUDGET=max budget was
    // WEAKER than the prod schedule at 6-8 players. One cordite, prod budget.
    const keys = new Set(sourceRoster().map(r => r.key));
    const dumped = new Set(kernelBotRoster().map(e => e.key));
    for (const gone of ['cordite_max', 'octogen_max', 'semtex_max']) {
        assert.ok(!keys.has(gone), `${gone} must not return to the roster`);
        assert.ok(!dumped.has(gone), `${gone} must not be in the kernel's roster`);
    }
    assert.equal(BotRegistry.find(r => r.key === 'cordite')!.knobs, 'CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75',
        'cordite plays on the production budget');
});

test('the shipped kernel carries the roster the source says, and links every seeded bot', () => {
    const roster = sourceRoster();
    const dump = new Map(kernelBotRoster().map(e => [e.key, e]));
    assert.deepEqual([...dump.keys()], roster.map(r => r.key), 'the kernel dumps the rows bots.c lists, in tier order');
    for (const entry of roster) {
        const k = dump.get(entry.key)!;
        assert.equal(k.usesLogs, entry.uses_logs === 1,
            `${entry.key}: uses_logs differs - the server would skip or feed the belief log wrongly`);
        assert.equal(k.seeded, entry.seeded === 1, `${entry.key}: seeded differs from bots.c`);
        assert.equal(k.offline, entry.offline === 1, `${entry.key}: offline differs from bots.c`);
        assert.equal(k.tier, entry.tier, `${entry.key}: tier differs from bots.c`);
        // A seeded bot is dealt on the site: the shipped module must run its
        // brain, or a seat carrying it has no bot behind its name.
        if (entry.seeded) assert.ok(k.linked, `${entry.key} is seeded but not linked into the shipped module`);
    }
});

test('every seeded bot has a city, which its server accounts are named after', () => {
    // The server's bot rows are "%<city> 1".."%<city> 7" with the row's own ids
    // (bot_seed.c), so a seeded row with no city could not be dealt at all; and
    // a city on an unseeded row would put a rung in the phone's picker that the
    // site never seats. tests.c holds the ids; this holds the pairing.
    // Mutation that turned it red (restored after): gunpowder given a city in bots.c.
    const roster = sourceRoster();
    const wrong = roster.filter(r => (r.seeded === 1) !== !!r.city).map(r => r.key);
    assert.deepEqual(wrong, [], 'these named bots have a city without being seeded, or are seeded without one');
});
