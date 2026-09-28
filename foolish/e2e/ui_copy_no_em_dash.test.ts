/* =============================================================================
 * The web's copy has no em dash
 * =============================================================================
 * The owner's rule: no em dash in anything a person reads (it reads as machine
 * written). A plain hyphen goes where one was. This walks every source file under
 * src/ with the TypeScript parser and fails on an em dash in a string literal, a
 * template literal or JSX text: the strings tables, the tutorial's copy, the
 * placeholders a screen draws, the page titles a link unfurls to. Comments are
 * not copy, and neither is a message only the console sees (an argument of a
 * console call).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import ts from 'typescript';

const ROOT = new URL('..', import.meta.url).pathname;
const SRC = join(ROOT, 'src');
const EM_DASH = '—';

const walk = (dir: string): string[] => readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? walk(path) : [path];
});

const inConsoleCall = (node: ts.Node): boolean => {
    for (let n: ts.Node | undefined = node.parent; n; n = n.parent) {
        if (ts.isCallExpression(n) && ts.isPropertyAccessExpression(n.expression)
            && ts.isIdentifier(n.expression.expression) && n.expression.expression.text === 'console') return true;
    }
    return false;
};

/** Every em dash a reader could see in `file`, as "path:line: text". */
export function emDashesIn(file: string, text: string): string[] {
    const kind = file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS;
    const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, kind);
    const found: string[] = [];
    const visit = (node: ts.Node): void => {
        const copy = ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node) || ts.isTemplateHead(node)
            || ts.isTemplateMiddle(node) || ts.isTemplateTail(node) || ts.isJsxText(node);
        if (copy && node.getText(source).includes(EM_DASH) && !inConsoleCall(node)) {
            const { line } = source.getLineAndCharacterOfPosition(node.getStart(source));
            found.push(`${relative(ROOT, file)}:${line + 1}: ${node.getText(source).trim().slice(0, 100)}`);
        }
        ts.forEachChild(node, visit);
    };
    visit(source);
    return found;
}

test('the scan sees copy and skips comments and the console', () => {
    const sample = [
        '// a comment — not copy',
        'const a = "one — two";',
        'const b = `x — ${a}`;',
        'console.error("for the console — only");',
        'const c = <p>jsx — text</p>;',
    ].join('\n');
    const hits = emDashesIn(join(SRC, 'sample.tsx'), sample);
    assert.deepEqual(hits.map((h) => h.split(':')[1]), ['2', '3', '5']);
});

test('no em dash in any string or JSX text under src/', () => {
    const hits = walk(SRC)
        .filter((f) => /\.(ts|tsx)$/.test(f))
        .flatMap((f) => emDashesIn(f, readFileSync(f, 'utf8')));
    assert.deepEqual(hits, [], `em dashes in user-facing copy:\n${hits.join('\n')}`);
});
