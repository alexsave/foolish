// No JSON in a shipped path.
//
// The rule is the owner's: JSON is slow to parse and slow to format, so a
// shipped path crosses its boundaries as bytes whose layout one C struct owns
// (structgen writes the readers and writers), and JSON is kept only where a
// third party forces it or for throwaway debugging output. The requests, the
// refusals and the realtime payload were the last of ours to go (c/src/ask.h,
// c/src/evwire.h the cast); this is what keeps them, and everything else, from
// coming back.
//
// It scans every tracked source file of every product in the monorepo that is
// not a test, a tool, a script or a document, for the calls that parse or
// format JSON, in the three languages a client is written in. Every hit must
// be in ALLOWED below, at exactly the count stated, with the reason it stays.
// A new hit fails. A second hit in an allowed file fails. An allowed hit that
// is gone fails too, so the list cannot outlive what it excuses.
//
// A line that only MENTIONS JSON in a comment is not a hit: comments are
// stripped first. (Several files explain what they replaced, and should.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const MONO = resolve(import.meta.dirname, '../../..');

/** The calls that parse or format JSON: JS/TS (JSON.x, .json(), Response.json), Swift (Foundation's three), Kotlin/Java (the usual libraries). */
const JSON_USE = /\bJSON\s*\.\s*[A-Za-z_]+|\.json\s*\(|\bJSON(?:Decoder|Encoder|Serialization)\b|\b(?:JSONObject|JSONArray|Gson|kotlinx\.serialization\.json)\b/g;

const SOURCE = /\.(?:ts|tsx|mts|cts|js|jsx|mjs|cjs|swift|kt|kts|java|html)$/;

/**
 * Not shipped: tests, tools, scripts, documents, experiments, generated
 * modules, CI. A directory NAME anywhere in the path, so the rule is the same
 * in every product.
 */
const NOT_SHIPPED_DIR = new Set([
    'e2e', 'test', 'tests', 'docs', 'scripts', 'tools', 'Tools', 'experiments', 'bench', 'bench_results',
    'gen', 'node_modules', '.github', 'media', 'Fixtures', 'fixtures', 'foolyard', 'cnitro', 'rust',
]);
const notShipped = (path: string): boolean => {
    const parts = path.split('/');
    if (parts.some((p) => NOT_SHIPPED_DIR.has(p) || /Tests$/.test(p) || /UITests$/.test(p))) return true;
    const name = parts[parts.length - 1];
    return /\.(?:test|spec|config)\.[a-z]+$/.test(name);
};

interface Allowed {
    /** Repo-relative path. */
    file: string;
    /** Hits in the file, exactly. */
    count: number;
    /** Why JSON stays here: who forces it, or why it is not a shipped path. */
    why: string;
    /** Another change in flight removes these: the file may have fewer hits than `count`, or none. Delete the entry once it lands. */
    leaving?: boolean;
}

const ADAPTER = 'foolish/server/impls/supabase/functions/_shared/adapter';
const ALLOWED: Allowed[] = [
    {
        file: `${ADAPTER}/utils.ts`, count: 1,
        why: 'FORCED by Supabase: the Realtime broadcast REST endpoint takes a JSON envelope { messages: [{ topic, event, payload, private }] } and a payload must be an object. Ours is { b }, one base64 string holding the kernel\'s cast.',
    },
    {
        file: `${ADAPTER}/auth.ts`, count: 4,
        why: 'Three are FORCED by the JWT format (RFC 7519): a token\'s header and claims are base64url JSON - two parses verify a token, one reads the unverified `sub` for a parallel read. The fourth is the JWKS fetch (res.json() on /auth/v1/.well-known/jwks.json), which a key pinned in the function\'s configuration would remove.',
    },
    {
        file: 'foolish/ios/FoolishNet/GameFeed.swift', count: 2,
        why: 'FORCED by Supabase: Realtime delivers a postgres_changes ROW as JSON, and supabase-swift\'s decodeRecord takes a JSONDecoder. The row\'s `view` column is the kernel\'s bytes, as hex.',
    },
    {
        file: 'foolish/src/components/ErrorBoundary.tsx', count: 2,
        why: 'Throwaway debugging output: the last ten render errors, kept in this browser\'s localStorage for a developer to read. Nothing is sent and nothing reads it back.',
    },
    {
        file: 'foolish/offlinefun/sw.js', count: 1,
        why: 'Throwaway debugging output: the service worker\'s own error log in localStorage, same as ErrorBoundary.',
    },
];

/** Source with its comments blanked, line structure kept: // to end of line, and block comments. Strings are left alone, so a "//" inside one (a URL) must not start a comment. */
function stripComments(src: string): string {
    let out = '';
    let i = 0;
    let quote = '';
    while (i < src.length) {
        const c = src[i], d = src[i + 1];
        if (quote) {
            out += c;
            if (c === '\\') { out += d ?? ''; i += 2; continue; }
            if (c === quote || (c === '\n' && quote !== '`')) quote = '';
            i++;
        } else if (c === '"' || c === '\'' || c === '`') {
            quote = c; out += c; i++;
        } else if (c === '/' && d === '/') {
            while (i < src.length && src[i] !== '\n') i++;
        } else if (c === '/' && d === '*') {
            i += 2;
            while (i < src.length && !(src[i] === '*' && src[i + 1] === '/')) { if (src[i] === '\n') out += '\n'; i++; }
            i += 2;
        } else {
            out += c; i++;
        }
    }
    return out;
}

/** Every JSON call in `src`, as "line: text". */
export function jsonUses(src: string): string[] {
    const hits: string[] = [];
    stripComments(src).split('\n').forEach((line, n) => {
        for (const m of line.matchAll(JSON_USE)) hits.push(`${n + 1}: ${m[0]}`);
    });
    return hits;
}

function shippedSources(): string[] {
    const tracked = execFileSync('git', ['ls-files', '-z'], { cwd: MONO, encoding: 'utf8', maxBuffer: 64 << 20 }).split('\0');
    return tracked.filter((f) => f && SOURCE.test(f) && !notShipped(f));
}

test('the scan reads code and not comments, in each language', () => {
    assert.deepEqual(jsonUses('const a = JSON.parse(x);\nconst b = await res.json();\n'), ['1: JSON.parse', '2: .json(']);
    assert.deepEqual(jsonUses('let r = try JSONDecoder().decode(T.self, from: d)\nlet e = JSONEncoder()\nJSONSerialization.data(withJSONObject: o)'),
        ['1: JSONDecoder', '2: JSONEncoder', '3: JSONSerialization']);
    assert.deepEqual(jsonUses('return Response.json({ ok: true })'), ['1: .json(']);
    assert.deepEqual(jsonUses('// it used to JSON.stringify(events)\n/* JSON.parse(a)\n   res.json() */\nconst url = "https://x/y.json"; // .json()\n'), [],
        'a mention in a comment, or a .json file name, is not a use');
    assert.deepEqual(jsonUses('const u = "http://a//b"; JSON.stringify(u)'), ['1: JSON.stringify'], 'a // inside a string does not hide what follows');
});

test('the scan covers the shipped code of every product', () => {
    const files = shippedSources();
    // Canaries: if a root moves or the filter starts dropping one, the scan is
    // looking at nothing and would pass forever.
    for (const must of [
        'foolish/src/contexts/ServerContext.tsx',
        'foolish/src/state/askServer.ts',
        'foolish/sdk/ts/table/client_table.ts',
        'foolish/sdk/swift/AskWire.swift',
        `${ADAPTER}/utils.ts`,
        'foolish/server/impls/supabase/functions/bot-heartbeat/index.ts',
        'foolish/ios/FoolishNet/OnlineService.swift',
        'foolish/offlinefun/sw.js',
    ]) assert.ok(files.includes(must), `${must} is scanned`);
    for (const product of ['uttt', 'werewolf', 'chuiniu', 'pickemup', 'tallybones', 'shared']) {
        assert.ok(files.some((f) => f.startsWith(`${product}/`)), `${product}/ has shipped sources in the scan`);
    }
    assert.ok(!files.some((f) => f.includes('/e2e/') || f.includes('/docs/')), 'and no test or document is');
});

test('no shipped code parses or formats JSON, but for the uses listed with their reasons', () => {
    const allowed = new Map(ALLOWED.map((a) => [a.file, a]));
    assert.equal(allowed.size, ALLOWED.length, 'one entry per file');
    const problems: string[] = [];
    const seen = new Set<string>();
    for (const file of shippedSources()) {
        const hits = jsonUses(readFileSync(join(MONO, file), 'utf8'));
        if (hits.length === 0) continue;
        seen.add(file);
        const a = allowed.get(file);
        if (!a) {
            problems.push(`${file}: JSON in a shipped path, and no reason on record\n${hits.map((h) => `      ${h}`).join('\n')}`);
        } else if (a.leaving ? hits.length > a.count : hits.length !== a.count) {
            problems.push(`${file}: ${hits.length} JSON use(s), the list allows ${a.leaving ? 'at most' : 'exactly'} ${a.count}\n${hits.map((h) => `      ${h}`).join('\n')}`);
        }
    }
    for (const a of ALLOWED) {
        if (!seen.has(a.file) && !a.leaving) problems.push(`${a.file}: allowed ${a.count} JSON use(s) but has none - delete its entry`);
    }
    assert.deepEqual(problems, [],
        `\n${problems.join('\n')}\n\n`
        + 'A shipped path crosses a boundary as bytes one C struct owns (c/src/ask.h is the model: the struct, its\n'
        + 'encoder and decoder in C, the reader and writer generated by structgen). If a third party really does\n'
        + 'force JSON here, add the file to ALLOWED in e2e/validation/no_json_validation.test.ts with who forces it.\n');
});

test('every allowed use says why', () => {
    for (const a of ALLOWED) assert.ok(a.why.length > 40, `${a.file}: a reason, not a label`);
});
