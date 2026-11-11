// The tests' way to load the site's own TypeScript modules under node: each is
// transpiled with the site's compiler, with every relative module it imports,
// as .mjs under a scratch directory, so the module under test is the page's
// file, not a copy. It lives outside test/, where node --test runs every .mjs.
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { tmpdir } from 'node:os';
import { pathToFileURL } from 'node:url';
import ts from 'typescript';

export const web = join(import.meta.dirname, '..');

/** Transpile `file` and every relative module it imports, as .mjs under out/. */
function transpile(out, file, seen) {
    if (seen.has(file)) return;
    seen.add(file);
    let js = ts.transpileModule(readFileSync(file, 'utf8'), {
        compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
    }).outputText;
    js = js.replace(/(from\s+['"])(\.{1,2}\/[^'"]+)(['"])/g, (_, a, spec, b) => {
        transpile(out, join(dirname(file), spec + '.ts'), seen);
        return a + spec + '.mjs' + b;
    });
    const to = join(out, relative(web, file)).replace(/\.ts$/, '.mjs');
    mkdirSync(dirname(to), { recursive: true });
    writeFileSync(to, js);
}

/** The module at `rel` (from uttt/web, say "lib/arena.ts"), imported. */
export async function loadTs(rel) {
    const out = mkdtempSync(join(tmpdir(), 'uttt-web-'));
    transpile(out, join(web, rel), new Set());
    return import(pathToFileURL(join(out, rel.replace(/\.ts$/, '.mjs'))).href);
}
