// The translated strings live in C, in c/i18n, and that raises exactly one
// question worth a gate: does any of it reach the browser?
//
// It must not. Twenty-five languages of the app's text is around 150 KB raw,
// and sdk/ts/wasm/bots.wasm.gz is downloaded by every visitor to the site
// against an 80 KiB cap with a few hundred bytes of headroom
// (e2e/mem/wasm_memory.test.ts). c/i18n is deliberately outside c/src and named
// in no *_SRC list in c/Makefile, so nothing links it - but "nothing links it"
// is a property of a build script, and a build script is a thing somebody
// edits. This checks the ARTIFACT instead: the module that actually ships.
//
// The rest of the file holds the other end of the same arrangement together -
// that every language really does carry every key. One 2-D table made a missing
// key a compile error. Twenty-five separate files make it a silent empty string
// on a board, in a language nobody here reads, so `datagen --require-complete`
// took that job over at generation time and this says so again over the
// generated modules, which is what the hosts import.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { gunzipSync } from 'node:zlib';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const GEN = join(REPO, 'sdk/ts/gen/i18n');
const C_I18N = join(REPO, 'c/i18n');

/** The generated per-language module for `code`, as a plain object. */
async function table(code: string): Promise<Record<string, string>> {
    const mod = await import(join(GEN, `strings.${code}.ts`));
    const key = Object.keys(mod).find((k) => k.startsWith('FoolishStrings'));
    assert.ok(key, `sdk/ts/gen/i18n/strings.${code}.ts exports no FoolishStrings* table`);
    return mod[key!];
}

async function languages(): Promise<{ code: string; display: string; rtl: number }[]> {
    const { FoolishLanguages } = await import(join(GEN, 'languages.ts'));
    return FoolishLanguages;
}

test('no translated string reaches the shipped wasm', async () => {
    const wasm = gunzipSync(readFileSync(join(REPO, 'sdk/ts/wasm/bots.wasm.gz')));
    // Three scripts and two directions, so this cannot pass by an encoding
    // accident: whatever put one of these in the module would put them all in.
    const langs = await languages();
    const samples: string[] = [];
    for (const { code } of langs) {
        if (!['ru', 'ko', 'he', 'ar', 'en'].includes(code)) continue;
        const t = await table(code);
        samples.push(t['ios.msg.pickseat'], t['ios.rules.goal.b']);
    }
    const found = samples.filter((s) => s && wasm.includes(Buffer.from(s, 'utf8')));
    assert.deepEqual(found, [],
        'these app strings are inside sdk/ts/wasm/bots.wasm.gz, which every visitor downloads.\n'
        + 'c/i18n is a build-time source of truth for tools/datagen and must be linked by nothing:\n'
        + `  ${found.join('\n  ')}\n`);
});

test('c/i18n is in no source list the C build compiles', () => {
    const mk = readFileSync(join(REPO, 'c/Makefile'), 'utf8');
    assert.ok(!/\bi18n\b/.test(mk),
        'c/Makefile names i18n. The string tables are read by tools/datagen and linked by nothing;\n'
        + 'compiling one puts 150 KB of translations into a module the browser downloads.');
});

test('every language in the registry has a C file and a generated module', async () => {
    const langs = await languages();
    assert.ok(langs.length >= 25, `the registry lists ${langs.length} languages`);
    const missing: string[] = [];
    for (const { code } of langs) {
        if (!existsSync(join(C_I18N, `strings_${code}.c`))) missing.push(`c/i18n/strings_${code}.c`);
        if (!existsSync(join(GEN, `strings.${code}.ts`))) missing.push(`sdk/ts/gen/i18n/strings.${code}.ts`);
    }
    assert.deepEqual(missing, [], `the registry (c/i18n/languages.h) names languages with no table:\n  ${missing.join('\n  ')}`);
});

test('no C language file is missing from the registry', async () => {
    const codes = new Set((await languages()).map((l) => l.code));
    const orphans = readdirSync(C_I18N)
        .map((f) => /^strings_([a-z]+)\.c$/.exec(f)?.[1])
        .filter((c): c is string => !!c && !codes.has(c));
    assert.deepEqual(orphans, [],
        'these language files exist but no row in c/i18n/languages.h names them, so nothing generates them:\n'
        + `  ${orphans.join('\n  ')}\n`);
});

async function keyList(): Promise<string[]> {
    const { FoolishStringKeys } = await import(join(GEN, 'keys.ts'));
    return Object.values(FoolishStringKeys);
}

test('every language carries every key', async () => {
    const keys = await keyList();
    assert.ok(keys.length >= 199, `the key list holds ${keys.length} keys`);
    const gaps: string[] = [];
    for (const { code } of await languages()) {
        const t = await table(code);
        for (const k of keys) if (typeof t[k] !== 'string') gaps.push(`${code}.${k}`);
    }
    assert.deepEqual(gaps, [],
        'these language/key pairs have no string. `datagen --require-complete` should have failed the\n'
        + 'build before this ran, so a failure here means that flag was dropped from tools/structgen/gen.sh:\n'
        + `  ${gaps.slice(0, 20).join('\n  ')}\n`);
});

test('no string is empty or only whitespace', async () => {
    // A blank cell renders as a blank button. It is the one failure that looks
    // like a layout bug rather than a missing translation, so it is checked as
    // its own thing rather than folded into key coverage.
    const keys = await keyList();
    const blank: string[] = [];
    for (const { code } of await languages()) {
        const t = await table(code);
        for (const k of keys) if (!(t[k] ?? '').trim()) blank.push(`${code}.${k}`);
    }
    assert.deepEqual(blank, [], `these strings are empty:\n  ${blank.join('\n  ')}\n`);
});

test('a placeholder a language drops is a placeholder the board never fills', async () => {
    // {name}-style holes are substituted by the host. A translation that lost
    // one renders "Waiting for" with nothing after it, which no test of key
    // coverage can see.
    const holes = (s: string) => [...new Set(s.match(/\{[a-zA-Z_]+\}/g) ?? [])].sort().join(',');
    const en = await table('en');
    const wrong: string[] = [];
    for (const { code } of await languages()) {
        if (code === 'en') continue;
        const t = await table(code);
        for (const [k, v] of Object.entries(en))
            if (holes(t[k] ?? '') !== holes(v)) wrong.push(`${code}.${k}: ${holes(v) || '(none)'} -> ${holes(t[k] ?? '') || '(none)'}`);
    }
    assert.deepEqual(wrong, [], `these translations do not carry English's placeholders:\n  ${wrong.join('\n  ')}\n`);
});

test('no em dash in a translated string', async () => {
    // The owner's rule, which e2e/ui_copy_no_em_dash.test.ts enforces over src/:
    // an em dash in copy reads as machine written, and a plain hyphen goes where
    // one was. That scan walks TypeScript, so when the tables moved to C the
    // tutorial's narration - which it used to cover - walked out from under it.
    // This is the same rule over the source the strings live in now, and over
    // twenty-five languages rather than three.
    //
    // ZHONGWEN IS THE EXEMPTION, and it is not a loophole: `——` is the Chinese
    // dash (破折号), a real mark of that language's own punctuation, always
    // doubled, and the twenty-seven in strings_zh.c are correct Chinese. The
    // rule is about English typography written by a machine, so it is asked of
    // the languages it means.
    const bad: string[] = [];
    for (const { code } of await languages()) {
        if (code === 'zh') continue;
        const t = await table(code);
        for (const [k, v] of Object.entries(t)) if (v.includes('—')) bad.push(`${code}.${k}: ${v}`);
    }
    assert.deepEqual(bad, [],
        'these translations contain an em dash. Use a plain hyphen:\n  ' + bad.join('\n  ') + '\n');
});

test('a language that localises the product name does not leave the English one behind', async () => {
    // The name is translated, not transliterated: ru "ДУРАЦКИЙ", ko "바보같은".
    // It is the adjective the game is named for, because Durak is Russian for
    // fool, so it is a word in each language rather than a token carried
    // through. These three keys are the ones that SAY the name - the About
    // heading, the paragraph that explains where the name came from, and the
    // iMessage invite - and a language that renders the name its own way must
    // render it its own way in all of them.
    //
    // Korean shipped exactly this contradiction: `foolish` was 바보같은 while
    // `about_foolish` read "FOOLISH 소개" and the invite read "Foolish - 탭하여
    // 참가", so the heading named a product the reader never saw anywhere else.
    //
    // Matched case-sensitively on purpose: the domain `foolish.cards` is an
    // address, not copy, and is not what this is looking for.
    const NAMES = ['about_foolish', 'about_paragraph_2', 'ios.msg.joininvite'];
    const en = await table('en');
    const stranded: string[] = [];
    for (const { code } of await languages()) {
        const t = await table(code);
        if (t['foolish'] === en['foolish']) continue;
        for (const k of NAMES)
            for (const form of [en['foolish'], 'Foolish'])
                if (t[k].includes(form)) stranded.push(`${code}.${k} still says "${form}", but ${code}.foolish is "${t['foolish']}"`);
    }
    assert.deepEqual(stranded, [],
        'these strings name the product in English in a language that has its own name for it:\n  '
        + stranded.join('\n  ') + '\n');
});
