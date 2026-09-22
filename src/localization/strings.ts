// The site's strings - the type, the English floor, and how a language is
// loaded. THE TABLE ITSELF IS NOT HERE ANY MORE.
//
// It used to be: three languages and a hundred and sixty keys written out in
// this file, while the phone app carried twenty-five languages and its own
// table in Swift. Two tables, no seam. They shared ten key names and disagreed
// about sixteen of the thirty cells those ten covered, which is what a seam
// being absent looks like.
//
// The strings are C now - c/i18n/strings_<code>.c, one file per language, keys
// in c/i18n/keys.h - and tools/datagen writes this side and FoolishKit's from
// them at build time. Changing a string is editing the C.
//
// ONE MODULE PER LANGUAGE, LOADED ON DEMAND. English is imported statically: it
// is the prerendered shell's language and the fallback floor, so it has to be
// there synchronously. Every other language is a dynamic import, which webpack
// splits into its own chunk - so a visitor downloads the one language they
// read, not twenty-five. That is the whole reason the C is split per language
// too; see c/i18n/README.md.
import { FoolishStringsEn } from '@sdk/ts/gen/i18n/strings.en';
import { FoolishStringKeys } from '@sdk/ts/gen/i18n/keys';
import { FoolishLanguages } from '@sdk/ts/gen/i18n/languages';

/** Every key that exists, as a union - derived from the C, never restated. */
export type StringId = (typeof FoolishStringKeys)[number];

const IDS: ReadonlySet<string> = new Set(FoolishStringKeys);

/** Is `s` a key the C declares? For the one caller that composes a key out of
 *  kernel data rather than writing it down - a bot's city is `bot.<strategy
 *  key>` (src/common/botName.ts), and which rungs have one is a fact of
 *  c/i18n, not a list TypeScript should keep a second copy of. Everywhere else,
 *  write the literal and let tsc check it. */
export const isStringId = (s: string): s is StringId => IDS.has(s);

/** A language's table. Partial only in type: every language carries every key
 *  (`datagen --require-complete`), and `translate` still falls back rather than
 *  render `undefined` if that ever stops being true. */
export type StringTable = Partial<Record<StringId, string>>;

/** English, statically. The floor under every lookup. */
export const EN: StringTable = FoolishStringsEn;

/** The languages the C declares, with the name each calls itself. */
export const LANGUAGES = FoolishLanguages;

export type Language = string;

const CODES = new Set(FoolishLanguages.map((l) => l.code));
export const isLanguage = (s: string): s is Language => CODES.has(s);

/** Load one language's table. English needs no fetch; everything else is its
 *  own chunk. The template literal is deliberate - webpack builds the context
 *  from `strings.` and emits one chunk per language, so this cannot turn into
 *  "download them all" the way a single combined module would. */
export async function loadLanguage(code: Language): Promise<StringTable> {
    if (code === 'en' || !CODES.has(code)) return EN;
    const mod = await import(`../../sdk/ts/gen/i18n/strings.${code}`);
    const key = `FoolishStrings${code.charAt(0).toUpperCase()}${code.slice(1)}`;
    return (mod as Record<string, StringTable>)[key] ?? EN;
}

/** Look `id` up in `table`, fall back to English, and substitute {name} holes.
 *
 *  ALL occurrences, not the first. `String.replace` with a string needle
 *  replaces once, which this used to do - a string mentioning the same
 *  placeholder twice rendered the second one raw. FStrings.t on iOS has always
 *  replaced all of them, so this is also the two sides agreeing. */
export function translate(table: StringTable, id: StringId, params?: Record<string, string>): string {
    let s = table[id] ?? EN[id] ?? id;
    if (params) {
        for (const [k, v] of Object.entries(params)) s = s.split(`{${k}}`).join(v);
    }
    return s;
}
