// Nothing in shared/ may name a product.
//
// shared/ exists because four trees were maintaining the same files by hand:
// the structgen/datagen toolchain, the libclang driver under them, SHA-256 and
// the deal RNG. The whole value of the directory is that a fix made there is a
// fix everywhere, and that survives exactly as long as the code in it belongs
// to nobody in particular.
//
// IT DOES NOT DRIFT ALL AT ONCE. It drifts one word at a time, and the first
// words are already written: this move had to reword five of them, inherited
// from the days when these files lived under a product -
//
//   shared/c/sha256.h                       "...and libfoolish.a"
//   shared/tools/structgen/sg_kotlin.c      "The Foolish kernel and its..."
//   shared/tools/structgen/test/swift.sh    "...the library FoolishKit links"
//
// None of those broke a build. That is the point: a product name in shared/ is
// never a failure, it is a comment that is wrong for one of its two readers, and
// then a default, and then a hardcoded bundle id. The rig is the worked example
// of both ends of that: 904 lines of it had NO product reference at all and are
// in shared/rig now, while rig.sh alone had accumulated 105 and stays with the
// product until someone can verify it on a device.
//
// So the line is drawn at the cheap end, where a rename is a one-line diff.
//
// Pure test - no Postgres, no network, no build.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const SHARED = join(REPO, 'shared');

/** Build outputs and generated modules are not source and are not committed. */
const SKIP_DIR = new Set(['build', 'gen', 'node_modules', '__pycache__', '.git']);

function sourceFiles(dir: string): string[] {
    const out: string[] = [];
    for (const entry of readdirSync(dir)) {
        const p = join(dir, entry);
        if (statSync(p).isDirectory()) {
            if (!SKIP_DIR.has(entry)) out.push(...sourceFiles(p));
        } else {
            out.push(p);
        }
    }
    return out;
}

/** What a product is called, in every spelling a file might reach for. */
const PRODUCT = [
    // The game nouns. `wolf` is matched on a word boundary so `werewolf` is one
    // hit rather than two, and so an ordinary English word is not one at all.
    /\bdurak\b/i,
    /foolish/i,
    /werewolf/i,
    /\bwolf\b/i,
    // Bundle ids, App Groups and the reverse-DNS they are built from.
    /cards\.foolish/i,
    /group\.cards/i,
    // Schemes, targets and the Xcode/Swift names that follow a product around.
    /\bCFoolish\w*/,
    /\bFoolish\w*/,
    /\bCWerewolf\w*/,
    /\.xcodeproj/,
    /MessagesExtension/,
];

test('no file under shared/ names a product', () => {
    const files = sourceFiles(SHARED);
    assert.ok(files.length > 20, `found only ${files.length} files under shared/ - did it move?`);

    const bad: string[] = [];
    for (const file of files) {
        let text: string;
        try {
            text = readFileSync(file, 'utf8');
        } catch {
            continue;                       // a binary fixture is not prose
        }
        text.split('\n').forEach((line, i) => {
            for (const rx of PRODUCT) {
                if (rx.test(line)) {
                    bad.push(`${relative(REPO, file)}:${i + 1}: ${line.trim()}`);
                    return;                 // one report per line is enough
                }
            }
        });
    }
    assert.deepEqual(bad, [],
        'these lines under shared/ name a product. shared/ is compiled by more\n'
        + 'than one of them, so a name here is either wrong for somebody or about\n'
        + 'to be. Reword it, or move the file back to the product that owns it:\n  '
        + bad.join('\n  '));
});

test('shared/ holds the files both products actually build', () => {
    // A guard against the opposite failure: shared/ quietly emptying out, or a
    // move half-landing. These are the paths c/Makefile and werewolf/c/Makefile
    // both compile, and the toolchain both would run.
    const expected = [
        'shared/c/sha256.c', 'shared/c/sha256.h',
        'shared/c/deal_rng.c', 'shared/c/deal_rng.h',
        'shared/tools/llvm.mk',
        'shared/tools/sgcommon/sgc.c',
        'shared/tools/structgen/structgen.c',
        'shared/tools/structgen/Makefile',
        'shared/tools/datagen/datagen.c',
        'shared/c/i18n/languages.h',
        'shared/swift/PackedBytes.swift',
        // The rig's measurement half: MSE, bar charts, square detection, the
        // frame-window extractor and the accessibility driver. Zero product
        // references between them, which is why they could move while rig.sh
        // could not.
        'shared/rig/lib/mse.py',
        'shared/rig/lib/squares.py',
        'shared/rig/lib/window.sh',
        'shared/rig/lib/ax.py',
    ];
    const missing = expected.filter((p) => {
        try { return !statSync(join(REPO, p)).isFile(); } catch { return true; }
    });
    assert.deepEqual(missing, [], `these are meant to be shared but are not there:\n  ${missing.join('\n  ')}`);
});

test('the product does not keep its own copy of a shared file', () => {
    // The failure this move was made to end: two byte-identical copies, one of
    // which gets the next fix. A copy that comes BACK is the same bug, and it
    // reads as innocent - a file appearing where it used to live.
    const shadowed = [
        'c/src/sha256.c', 'c/src/sha256.h',
        'c/src/deal_rng.c', 'c/src/deal_rng.h',
        'sdk/swift/PackedBytes.swift',
        'tools/llvm.mk',
        'tools/sgcommon',
        'tools/datagen',
        'c/i18n/languages.h',
        'uttt/c/i18n/languages.h',
        'tools/structgen/structgen.c',
        'werewolf/c/src/sha256.c',
        'werewolf/c/src/deal_rng.c',
        'werewolf/tools',
    ];
    const back: string[] = [];
    for (const p of shadowed) {
        try { statSync(join(REPO, p)); back.push(p); } catch { /* gone, as it should be */ }
    }
    assert.deepEqual(back, [],
        'these live in shared/ now, and a second copy has reappeared beside a\n'
        + 'product. One of the two will get the next fix and the other will not:\n  '
        + back.join('\n  '));
});
