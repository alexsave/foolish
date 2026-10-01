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

// Rooted at the REPOSITORY, where foolish/, uttt/, werewolf/ and shared/ sit side
// by side: this gate is about the line between them, so it cannot live inside one.
const REPO = resolve(import.meta.dirname, '../../..');
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
    // The shedding game, in the folder name and in the title's spellings
    // ("Pick 'Em Up", "Pick Em Up", "pickemup").
    /pickemup/i,
    /pick ?'?em ?up/i,
    // Liar's Dice, in the folder name and the title ("Chui Niu", "chuiniu").
    /chui ?niu/i,
    // The dice solitaire, in the folder name and the title's spellings
    // ("Tally Bones", "TallyBones", "tallybones"); unanchored, so its targets
    // (TallybonesKit, TallybonesMessages) are hits too. A bare "tally" is not:
    // it is an ordinary word a shared counter may well use.
    /tally ?bones/i,
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
        'shared/c/mixrad.c', 'shared/c/mixrad.h',   // the mixed-radix arithmetic under two game coders
        'shared/c/b32.c', 'shared/c/b32.h',         // the base32 code alphabet over it
        'shared/tools/llvm.mk',
        'shared/scripts/ci_llvm.sh',   // the pinned toolchain, for every product's lanes
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

/**
 * Every file in shared/c, at every place a product keeps its C, under every
 * product. Spelled out as a cross product rather than by hand so a sixth
 * product, or a fifth shared module, is one word here and not twenty lines.
 */
function productCopies(): string[] {
    const products = ['foolish', 'uttt', 'pickemup', 'chuiniu', 'tallybones', 'werewolf'];
    // The flat modules: a source and its header, wherever a product's C lives.
    const flat = ['sha256', 'deal_rng', 'b32', 'mixrad'].flatMap((m) => [`${m}.c`, `${m}.h`]);
    const flatDirs = ['c', 'c/src', 'c/tests', 'c/bot'];
    // The header-only modules and the registry, where a copy would most likely land.
    const headers = [
        'c/src/msg_stage.h', 'c/msg_stage/msg_stage.h',
        'c/src/motion_ruler.h', 'c/motion_ruler/motion_ruler.h',
        'c/src/languages.h', 'c/i18n/languages.h',
        'c/src/le_bytes.h',
    ];
    // The freestanding libc and libm. NOT wasm/include/stdio.h: shared/c/wasm's
    // own stdio.h says a build that needs a different stdio keeps its own, and
    // the card kernel does (c/wasm/include/stdio.h, fprintf for research builds).
    const wasm = ['c/wasm/libc.c', 'c/wasm/libm.c', 'c/wasm/include/string.h', 'c/wasm/include/math.h'];
    // THE ONE KNOWN COPY, left out by name so it is not a silent hole. The third
    // product forked before the registry moved to shared/ and still reads its
    // own (its i18n_source_of_truth test and its datagen are rooted at it). It
    // is paused and out of this gate's reach; retiring it is its own change,
    // and deleting this line is how that change proves itself.
    const known = new Set(['werewolf/c/i18n/languages.h']);
    const out: string[] = [];
    for (const p of products) {
        for (const d of flatDirs) for (const f of flat) out.push(`${p}/${d}/${f}`);
        for (const h of [...headers, ...wasm]) out.push(`${p}/${h}`);
    }
    return out.filter((p) => !known.has(p));
}

test('the product does not keep its own copy of a shared file', () => {
    // The failure this move was made to end: two byte-identical copies, one of
    // which gets the next fix. A copy that comes BACK is the same bug, and it
    // reads as innocent - a file appearing where it used to live.
    const shadowed = [
        'foolish/c/src/sha256.c', 'foolish/c/src/sha256.h',
        'foolish/c/src/deal_rng.c', 'foolish/c/src/deal_rng.h',
        'foolish/sdk/swift/PackedBytes.swift',
        'foolish/tools/llvm.mk',
        'foolish/tools/sgcommon',
        'foolish/tools/datagen',
        'foolish/c/i18n/languages.h',
        'uttt/c/i18n/languages.h',
        'foolish/tools/structgen/structgen.c',
        'foolish/scripts/ci_llvm.sh',
        'scripts/ci_llvm.sh',
        'werewolf/c/src/sha256.c',
        'werewolf/c/src/deal_rng.c',
        'werewolf/tools',
        ...productCopies(),
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
