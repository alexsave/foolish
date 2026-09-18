# c/i18n - the app's text, in C

Every string the phone app, the iMessage board and the website render, in one place, as C data.
`tools/datagen` reads it at build time and writes the TypeScript the web imports and the Swift FoolishKit compiles.
Nothing here is compiled into anything that ships.

## Why C

Because the alternative had already failed.
The phone app carried twenty-five languages and one hundred and ninety-nine keys in an in-code Swift table -
5,063 lines, the largest hand-written file in the repo by more than double -
while the website carried three languages and its own hundred-and-sixteen-key table in TypeScript.
Two tables, no seam, and a string changed on the board stayed changed only on the board.

Milestone E4 proposed the other direction: the website's `strings.ts` as the source, merged into an Xcode String Catalog.
It never landed, the in-code Swift table won instead, and the catalog it would have written went stale and had to be excluded from the build (`ios/project.yml`).
The lesson taken here is that neither host should be the other's upstream.
Both are generated, from this.

## The shape

| file | what it is |
| --- | --- |
| `keys.h` | every key that exists, as an enum, plus `FS_KEY_NAME[]` - the name each slot answers to |
| `languages.h` | the registry: one row per language, with the name it calls itself and whether it is written right to left |
| `strings_<code>.c` | one language's table, `[FS_K_...] = "..."` |

One file per language, not one grid, and that is a bundle decision.
A dynamic import keeps every export of its target alive in the web bundle whatever the importer uses -
`src/wasm/msgKernel.ts` learned this the expensive way -
so the module the site imports for a language has to *be* one language.
Twenty-five in one module would ship all of them to every visitor.

It also makes adding a language additive: a `strings_<code>.c` and a row in `languages.h`.
`tools/structgen/gen.sh` reads the registry to decide what to generate, so no build script and no CI lane keeps a list that can fall behind.

## The thing that keeps twenty-five files honest

A single 2-D table made a missing key a compile error.
Twenty-five independent tables make it a silent empty string, on a board, in a language nobody here reads.

`datagen --require-complete` took that job over.
Every language's table must fill every slot of `FsKey`, and a gap fails the build naming each missing key.
`e2e/validation/i18n_source_of_truth.test.ts` says it again over the generated modules, and also checks that no translation lost a `{placeholder}` English has.

## Nothing here ships

`c/i18n` is outside `c/src` and appears in no `*_SRC` list in `c/Makefile`, on purpose.
Twenty-five languages is around 150 KB of string data, and `sdk/ts/wasm/bots.wasm.gz` is downloaded by every visitor to the site against an 80 KiB cap with a few hundred bytes of headroom (`e2e/mem/wasm_memory.test.ts`).

That is a property of a build script, and a build script is a thing somebody edits, so the gate checks the artifact instead:
`e2e/validation/i18n_source_of_truth.test.ts` decompresses the shipped module and looks for the strings.

## Changing a string

Edit the language file.
That is the whole procedure - the generated modules are build outputs, are not committed, and are rewritten by every lane that compiles or loads them (`npm run gen`).

Adding a *key* means a line in `keys.h` and a line in all twenty-five language files.
That is deliberate friction: a key that exists in one language and not the others is the failure this whole arrangement is built to make impossible, so the build refuses it rather than shipping it.
