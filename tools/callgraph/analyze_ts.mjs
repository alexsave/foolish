// TypeScript/TSX analyzer using the real TypeScript compiler API.
// Emits { nodes:[], edges:[] } in the common graph shape.
import ts from 'typescript';
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';

const ROOT = path.resolve(process.argv[2]);
const OUT = process.argv[3];

const files = execSync('git ls-files', { cwd: ROOT, maxBuffer: 1 << 28 })
  .toString().split('\n')
  .filter(f => /\.(ts|tsx|mts|mjs)$/.test(f))
  .filter(f => !f.includes('node_modules'))
  .map(f => path.join(ROOT, f));

const options = {
  target: ts.ScriptTarget.ES2022,
  module: ts.ModuleKind.ESNext,
  moduleResolution: ts.ModuleResolutionKind.Bundler,
  jsx: ts.JsxEmit.ReactJSX,
  allowJs: true,
  checkJs: false,
  allowImportingTsExtensions: true,
  resolveJsonModule: true,
  skipLibCheck: true,
  noResolve: false,
  noEmit: true,
  baseUrl: ROOT,
  paths: {
    '@shared/*': ['./server/impls/supabase/functions/_shared/*'],
    '@api/*': ['./server/api/*'],
    '@sdk/*': ['./sdk/*'],
  },
  esModuleInterop: true,
  allowSyntheticDefaultImports: true,
};

const program = ts.createProgram(files, options);
const checker = program.getTypeChecker();

const nodes = new Map(); // id -> node
const edges = new Map(); // key -> edge
const wanted = new Set(files.map(f => path.relative(ROOT, f)));

function rel(fileName) { return path.relative(ROOT, fileName); }

function declId(decl) {
  const sf = decl.getSourceFile();
  if (!sf) return null;
  const r = rel(sf.fileName);
  if (!wanted.has(r)) return null;
  const nm = nameOfDecl(decl);
  if (!nm) return null;
  return `ts:${r}#${nm}`;
}

function nameOfDecl(decl) {
  // Build a qualified-ish name: Class.method or plain name
  let base = null;
  if (decl.name && ts.isIdentifier(decl.name)) base = decl.name.text;
  else if (decl.name && ts.isStringLiteral(decl.name)) base = decl.name.text;
  else if (decl.name && ts.isPrivateIdentifier(decl.name)) base = decl.name.text;
  else if (ts.isConstructorDeclaration(decl)) base = 'constructor';
  else if ((ts.isArrowFunction(decl) || ts.isFunctionExpression(decl)) && decl.parent) {
    const p = decl.parent;
    if (ts.isVariableDeclaration(p) && ts.isIdentifier(p.name)) base = p.name.text;
    else if (ts.isPropertyAssignment(p) && p.name && (ts.isIdentifier(p.name) || ts.isStringLiteral(p.name))) base = p.name.text;
    else if (ts.isPropertyDeclaration(p) && p.name && ts.isIdentifier(p.name)) base = p.name.text;
    else if (ts.isExportAssignment(p)) base = 'default';
  }
  if (!base) return null;
  // prefix with class/interface owner when present
  let owner = null;
  let cur = decl.parent;
  while (cur) {
    if (ts.isClassDeclaration(cur) || ts.isClassExpression(cur) || ts.isInterfaceDeclaration(cur)) {
      if (cur.name) { owner = cur.name.text; break; }
    }
    cur = cur.parent;
  }
  return owner ? `${owner}.${base}` : base;
}

function isFnLike(n) {
  return ts.isFunctionDeclaration(n) || ts.isMethodDeclaration(n) || ts.isConstructorDeclaration(n)
    || ts.isGetAccessorDeclaration(n) || ts.isSetAccessorDeclaration(n)
    || ts.isArrowFunction(n) || ts.isFunctionExpression(n);
}

function isExported(decl) {
  let n = decl;
  while (n) {
    if (n.modifiers && n.modifiers.some(m => m.kind === ts.SyntaxKind.ExportKeyword)) return true;
    if (ts.isVariableStatement(n) && n.modifiers && n.modifiers.some(m => m.kind === ts.SyntaxKind.ExportKeyword)) return true;
    if (ts.isSourceFile(n)) break;
    n = n.parent;
  }
  return false;
}

function addNode(id, o) {
  if (!nodes.has(id)) nodes.set(id, o);
  return nodes.get(id);
}

function addEdge(s, t, conf) {
  if (!s || !t || s === t) return;
  const k = s + '\\u0000' + t;
  const prev = edges.get(k);
  const rank = { high: 3, med: 2, low: 1 };
  if (!prev || rank[conf] > rank[prev.conf]) edges.set(k, { s, t, conf });
}

// Pass 1: collect all function-like definitions
const sfs = program.getSourceFiles().filter(sf => !sf.isDeclarationFile && wanted.has(rel(sf.fileName)));
for (const sf of sfs) {
  const r = rel(sf.fileName);
  const visit = (n) => {
    if (isFnLike(n)) {
      const nm = nameOfDecl(n);
      if (nm) {
        const id = `ts:${r}#${nm}`;
        const { line } = sf.getLineAndCharacterOfPosition(n.getStart(sf));
        addNode(id, {
          id, name: nm, file: r, line: line + 1, lang: 'ts',
          kind: ts.isMethodDeclaration(n) || ts.isConstructorDeclaration(n) ? 'method'
            : (ts.isArrowFunction(n) || ts.isFunctionExpression(n)) ? 'arrow' : 'function',
          exported: isExported(n),
          loc: sf.getLineAndCharacterOfPosition(n.getEnd()).line - line + 1,
        });
      }
    }
    ts.forEachChild(n, visit);
  };
  ts.forEachChild(sf, visit);
}

// Pass 2: walk calls, attribute to nearest named enclosing function (or file-level pseudo node)
const unresolvedNodes = new Map();

function enclosingId(n, r, sf) {
  let cur = n.parent;
  while (cur && !ts.isSourceFile(cur)) {
    if (isFnLike(cur)) {
      const nm = nameOfDecl(cur);
      if (nm) return `ts:${r}#${nm}`;
    }
    cur = cur.parent;
  }
  // module top level
  const id = `ts:${r}#<module>`;
  if (!nodes.has(id)) {
    addNode(id, { id, name: '<module>', file: r, line: 1, lang: 'ts', kind: 'module-init', exported: false, loc: 0 });
  }
  return id;
}

function resolveCallee(expr) {
  let sym = checker.getSymbolAtLocation(expr);
  if (sym && sym.flags & ts.SymbolFlags.Alias) {
    try { sym = checker.getAliasedSymbol(sym); } catch { }
  }
  if (!sym) return { id: null, label: fallbackLabel(expr) };
  const decls = sym.getDeclarations() || [];
  for (const d of decls) {
    let target = d;
    if (ts.isVariableDeclaration(d) && d.initializer && isFnLike(d.initializer)) target = d.initializer;
    if (ts.isPropertyAssignment(d) && d.initializer && isFnLike(d.initializer)) target = d.initializer;
    if (ts.isPropertyDeclaration(d) && d.initializer && isFnLike(d.initializer)) target = d.initializer;
    const id = declId(target);
    if (id && nodes.has(id)) return { id, label: null };
    if (id) return { id, label: null, weak: true };
  }
  return { id: null, label: sym.getName() || fallbackLabel(expr) };
}

function fallbackLabel(expr) {
  if (ts.isIdentifier(expr)) return expr.text;
  if (ts.isPropertyAccessExpression(expr)) {
    const o = expr.expression;
    const on = ts.isIdentifier(o) ? o.text : (ts.isThisExpression(o) ? 'this' : '?');
    return `${on}.${expr.name.text}`;
  }
  return '<dynamic>';
}

const BUILTIN = /^(console|Math|JSON|Object|Array|String|Number|Boolean|Promise|Map|Set|Date|RegExp|Error|Symbol|BigInt|Uint8Array|Int8Array|Int32Array|Uint32Array|Float64Array|ArrayBuffer|DataView|WeakMap|WeakSet|Reflect|Proxy|globalThis|process|Buffer|performance|crypto|TextEncoder|TextDecoder|URL|URLSearchParams|Intl)\b/;

for (const sf of sfs) {
  const r = rel(sf.fileName);
  const visit = (n) => {
    if (ts.isCallExpression(n) || ts.isNewExpression(n)) {
      const src = enclosingId(n, r, sf);
      const expr = n.expression;
      // `supabase.rpc('commit_game', {...})` is a call into a Postgres
      // routine. The checker resolves it to postgrest-js, which says nothing
      // about which routine - the string literal is the callee. merge.py
      // anchors `sqlrpc:` on the SQL graph.
      if (ts.isPropertyAccessExpression(expr) && expr.name.text === 'rpc' &&
          n.arguments.length && ts.isStringLiteralLike(n.arguments[0])) {
        addEdge(src, 'sqlrpc:' + n.arguments[0].text, 'cross');
      }
      let key = expr;
      if (ts.isPropertyAccessExpression(expr)) key = expr.name;
      else if (ts.isElementAccessExpression(expr)) key = null;
      if (key) {
        const res = resolveCallee(key);
        if (res.id && nodes.has(res.id)) {
          addEdge(src, res.id, 'high');
        } else if (res.id) {
          addEdge(src, res.id, 'med');
        } else if (res.label && res.label !== '<dynamic>') {
          const lbl = res.label;
          const root = ts.isPropertyAccessExpression(expr) && ts.isIdentifier(expr.expression) ? expr.expression.text : lbl;
          if (!BUILTIN.test(root) && !BUILTIN.test(lbl)) {
            const uid = `ext:${lbl}`;
            if (!unresolvedNodes.has(uid)) {
              unresolvedNodes.set(uid, { id: uid, name: lbl, file: '(unresolved)', line: 0, lang: 'unresolved', kind: 'unresolved', exported: false, loc: 0 });
            }
            addEdge(src, uid, 'low');
          }
        }
      }
    }
    ts.forEachChild(n, visit);
  };
  ts.forEachChild(sf, visit);
}

for (const [k, v] of unresolvedNodes) if (!nodes.has(k)) nodes.set(k, v);

fs.writeFileSync(OUT, JSON.stringify({ nodes: [...nodes.values()], edges: [...edges.values()] }));
console.error(`ts: ${nodes.size} nodes, ${edges.size} edges over ${sfs.length} files`);
