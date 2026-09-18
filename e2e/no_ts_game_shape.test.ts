// No TypeScript knows the game's shape or a kernel buffer's byte layout
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 8).
//
// The C kernel owns the board and every layout that crosses into or out of it.
// Where TS touches a kernel value it goes through the modules tools/structgen
// generates from the real wasm32 layouts (sdk/ts/gen/). Two static checks hold
// that, over every TypeScript file under server/, sdk/, src/, e2e/, scripts/ and
// tools/, read with the TypeScript parser (not a regex over the text):
//
//   1. No interface or object type declares two or more of the game's own
//      fields: table_battles, deck_length, power_suit, good_players,
//      elimination_order. One alone is an ordinary word (a column name, a
//      request field); two together are a TS Game coming back.
//
//   2. No file that reads or writes a wasm instance's linear memory assembles a
//      multi-byte value by hand: no DataView, no DataView-style get/set call, and
//      no shift by 4, 8, 16 or 24 bits (a nibble or a byte of a layout). A layout
//      read through a generated accessor needs none of them. Bytes that never
//      touch kernel memory (base64 and hex, gzip, the realtime JSON envelope, a
//      hash) are not a kernel layout and are not scanned. The functions still
//      doing it are named below, each with the reason, by `file#function`.
//
// Out of scope by path:
//   - sdk/ts/gen/ and tools/structgen/: the generated modules, and structgen
//     itself with its tests, which hold the generated accessors to the C layouts
//     byte for byte; the one place allowed to know layouts.
//   - c/: the kernel itself.
//   - offlinefun/: research and PWA leftovers outside the product tree, never
//     loaded by a server, the web or a test.
//   - node_modules, build and .next output.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';
import ts from 'typescript';

const REPO = resolve(import.meta.dirname, '..');
const ROOTS = ['server', 'sdk', 'src', 'e2e', 'scripts', 'tools'];
const SKIP_DIRS = new Set(['node_modules', 'build', '.next', 'fixtures']);
const GENERATED = ['sdk/ts/gen/', 'tools/structgen/'];

function sources(): string[] {
    const out: string[] = [];
    const walk = (dir: string) => {
        for (const name of readdirSync(dir)) {
            if (SKIP_DIRS.has(name)) continue;
            const p = join(dir, name);
            if (statSync(p).isDirectory()) walk(p);
            else if (/\.(ts|tsx|mts|cts|mjs|js)$/.test(name) && !name.endsWith('.d.ts')) out.push(p);
        }
    };
    for (const r of ROOTS) walk(join(REPO, r));
    return out
        .map((p) => relative(REPO, p))
        .filter((f) => !GENERATED.some((g) => f.startsWith(g)))
        .sort();
}

function parse(file: string): ts.SourceFile {
    const kind = file.endsWith('.tsx') ? ts.ScriptKind.TSX : /\.(mjs|js)$/.test(file) ? ts.ScriptKind.JS : ts.ScriptKind.TS;
    return ts.createSourceFile(file, readFileSync(join(REPO, file), 'utf8'), ts.ScriptTarget.Latest, true, kind);
}

// ---- 1. the game's shape ------------------------------------------------------

const GAME_FIELDS = ['table_battles', 'deck_length', 'power_suit', 'good_players', 'elimination_order'];

/** Every interface or object type literal in `sf` naming two or more game fields. */
export function gameShapes(sf: ts.SourceFile): string[] {
    const found: string[] = [];
    const visit = (node: ts.Node) => {
        if (ts.isInterfaceDeclaration(node) || ts.isTypeLiteralNode(node)) {
            const names = node.members
                .map((m) => (m.name && (ts.isIdentifier(m.name) || ts.isStringLiteral(m.name)) ? m.name.text : ''))
                .filter((n) => GAME_FIELDS.includes(n));
            if (names.length >= 2) {
                const line = sf.getLineAndCharacterOfPosition(node.getStart(sf)).line + 1;
                const label = ts.isInterfaceDeclaration(node) ? `interface ${node.name.text}` : 'type literal';
                found.push(`${sf.fileName}:${line} ${label} declares ${names.join(', ')}`);
            }
        }
        ts.forEachChild(node, visit);
    };
    visit(sf);
    return found;
}

// ---- 2. byte layouts on kernel memory -----------------------------------------

// A file reaches a wasm instance's linear memory through its exported memory
// or an exported buffer address (wasm_io_ptr, wasm_replay_io_ptr, ...).
const TOUCHES_KERNEL_MEMORY = /\bmemory\.buffer\b|\bwasm_\w*_ptr\s*\(|\b__mem\s*\(/;
const LAYOUT_SHIFTS = new Set([4, 8, 16, 24]);
const DATAVIEW_CALL = /^(get|set)(Int|Uint|Float|BigInt|BigUint)(8|16|32|64)$/;

function enclosingFunction(node: ts.Node): string {
    for (let n: ts.Node | undefined = node.parent; n; n = n.parent) {
        if ((ts.isFunctionDeclaration(n) || ts.isMethodDeclaration(n)) && n.name) return n.name.getText();
        if ((ts.isArrowFunction(n) || ts.isFunctionExpression(n)) && ts.isVariableDeclaration(n.parent)) return n.parent.name.getText();
    }
    return '(module)';
}

/** Every hand-made byte layout in `sf`, as `file#function: what`. */
export function layoutWrites(sf: ts.SourceFile): string[] {
    if (!TOUCHES_KERNEL_MEMORY.test(sf.text)) return [];
    const found: string[] = [];
    const hit = (node: ts.Node, what: string) => {
        const line = sf.getLineAndCharacterOfPosition(node.getStart(sf)).line + 1;
        found.push(`${sf.fileName}#${enclosingFunction(node)}: ${what} (line ${line})`);
    };
    const visit = (node: ts.Node) => {
        if (ts.isNewExpression(node) && ts.isIdentifier(node.expression) && node.expression.text === 'DataView') {
            hit(node, 'new DataView');
        } else if (ts.isCallExpression(node) && ts.isPropertyAccessExpression(node.expression)
            && DATAVIEW_CALL.test(node.expression.name.text)) {
            hit(node, node.expression.name.text);
        } else if (ts.isBinaryExpression(node)) {
            const op = node.operatorToken.kind;
            const shift = op === ts.SyntaxKind.LessThanLessThanToken || op === ts.SyntaxKind.GreaterThanGreaterThanToken
                || op === ts.SyntaxKind.GreaterThanGreaterThanGreaterThanToken
                || op === ts.SyntaxKind.LessThanLessThanEqualsToken || op === ts.SyntaxKind.GreaterThanGreaterThanEqualsToken
                || op === ts.SyntaxKind.GreaterThanGreaterThanGreaterThanEqualsToken;
            if (shift && ts.isNumericLiteral(node.right) && LAYOUT_SHIFTS.has(Number(node.right.text))) {
                hit(node, `${node.operatorToken.getText(sf)} ${node.right.text}`);
            }
        }
        ts.forEachChild(node, visit);
    };
    visit(sf);
    return found;
}

// Functions still laying bytes on kernel memory by hand, by `file#function`.
//
// Permanent: a test that must read the wire without the kernel's own reader.
const INDEPENDENT_READERS: Record<string, string> = {
    'e2e/security_hidden_info.test.ts#checkViewEnvelope':
        'S1 scans the raw envelope bytes a client receives for card identities it must not see; it finds the view '
        + 'by its length field itself, so a reader bug cannot hide a leak from the scan',
    'e2e/security_hidden_info.test.ts#walkEvwire':
        'S1 walks the raw push frames by their length fields for the same reason',
};

// Kernel entry points whose argument or result is still a hand-packed byte string
// rather than a C struct structgen generates. Entries are never added, and the
// eleven that were here are gone: every one of those entries now crosses as a C
// struct read or written through a generated accessor (the FMSG header as
// msg_wire.h MsgHeader, the legal-move menu as legal.h LegalMoves read where it
// lies, a replay refusal as replay.h ReplayError, a bot search's belief as
// bot_drive.h BeliefProbe, the share link's names and times as
// replay_extras.h ReplayExtras, a frames chunk as replay_steps.h
// ReplayFrameIndex, and the test entries' table and trailer as roster.h
// RosterSpec and RosterTrailerRead).
const NOT_YET_GENERATED: Record<string, string> = {};

const ALLOWED: Record<string, string> = { ...INDEPENDENT_READERS, ...NOT_YET_GENERATED };

// ---- the tests ----------------------------------------------------------------

test('the walk reads the product tree (the checks are not vacuous)', () => {
    const files = sources();
    assert.ok(files.length > 250, `found ${files.length} files`);
    for (const f of ['sdk/ts/wasm/bots.ts', 'sdk/ts/table/server_table.ts', 'src/app/providers.tsx', 'e2e/table_no_game_object.test.ts']) {
        assert.ok(files.includes(f), `the walk reaches ${f}`);
    }
    assert.ok(!files.some((f) => f.startsWith('sdk/ts/gen/') || f.startsWith('tools/structgen/') || f.startsWith('offlinefun/') || f.startsWith('c/')), 'generated, structgen, kernel and offlinefun files are out');
});

test('the detectors see what they are for', () => {
    const src = (text: string) => ts.createSourceFile('probe.ts', text, ts.ScriptTarget.Latest, true);
    assert.equal(gameShapes(src('interface G { deck_length: number; power_suit: number }')).length, 1);
    assert.equal(gameShapes(src('type G = { readonly table_battles: B[]; "good_players"?: string[] }')).length, 1);
    assert.equal(gameShapes(src('interface Row { deck_length: number; status: string }')).length, 0);
    assert.equal(gameShapes(src('const g = { deck_length: 1, power_suit: 2 };')).length, 0, 'a value is not a declaration');
    const mem = 'const ex = inst.exports; const b = new Uint8Array(ex.memory.buffer);';
    assert.deepEqual(layoutWrites(src(`${mem} function f() { return b[0] | (b[1] << 8); }`)), ['probe.ts#f: << 8 (line 1)']);
    assert.equal(layoutWrites(src(`${mem} const g = () => new DataView(b.buffer).getUint16(0, true);`)).length, 2);
    assert.equal(layoutWrites(src(`${mem} function h(x: number) { return (x >>> 0) ^ (1 << x) ^ (x << 13); }`)).length, 0, 'a mask or a hash is not a layout');
    assert.equal(layoutWrites(src('function io(b: Uint8Array) { return b[0] | (b[1] << 8); }')).length, 0, 'bytes that never touch kernel memory');
});

test('no TypeScript outside the generated modules declares the game\'s shape', () => {
    const offenders = sources().flatMap((f) => gameShapes(parse(f)));
    assert.deepEqual(offenders, [], `\n${offenders.join('\n')}\n`);
});

test('no TypeScript outside the generated modules lays out bytes on kernel memory by hand', () => {
    const offenders = sources()
        .flatMap((f) => layoutWrites(parse(f)))
        .filter((o) => !(o.slice(0, o.indexOf(':')) in ALLOWED));
    assert.deepEqual(offenders, [], `\n${offenders.join('\n')}\n`);
});

test('every named function still exists and still needs its entry; the temporary list only shrinks', () => {
    const used = new Set(sources().flatMap((f) => layoutWrites(parse(f))).map((o) => o.slice(0, o.indexOf(':'))));
    const stale = Object.keys(ALLOWED).filter((k) => !used.has(k));
    assert.deepEqual(stale, [], `drop from the lists: ${stale.join(', ')}`);
    assert.equal(Object.keys(NOT_YET_GENERATED).length, 0,
        'the temporary list is empty and stays empty: a kernel entry crosses as a struct, not as bytes a host packs');
});
