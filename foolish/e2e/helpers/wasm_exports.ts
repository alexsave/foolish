// The kernel's export table, read out of a linked module, and the TypeScript
// that binds it.
//
// Every host casts `instance.exports` to an interface structgen writes from the
// link line and the C definitions (sdk/ts/gen/exports.<link>.ts), so a call
// with a wrong name, arity or return is a compile error, and
// e2e/wasm_exports.test.ts holds each interface to its built module with the
// readers below. What is left for a scanner of hand-written TypeScript: the
// hand-typed interfaces that remain (BINDER_MODULES, a closed list) and the
// names the tree actually calls (calledExports), which is what tells an export
// with no caller from one that earns the code it roots.
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

/**
 * The names one of c/Makefile's `WASM_*_EXPORTS` lists declares, following the
 * `$(WASM_OTHER)` references it is built out of.
 *
 * The Makefile is the weaker source - it says what the link was ASKED for, not
 * what the module carries - so it is read for one thing only: the test build,
 * which is never committed and so cannot be read as a module in a lane without
 * a compiler.
 */
export function makefileExportNames(makefile: string, variable: string): string[] {
    const text = readFileSync(makefile, 'utf8').replace(/\\\n/g, ' ');
    const line = text.split('\n').find((l) => l.startsWith(`${variable} :=`) || l.startsWith(`${variable} =`));
    if (!line) throw new Error(`c/Makefile defines no ${variable}`);
    const refs = [...line.matchAll(/\$\((WASM_[A-Z_]+)\)/g)].map((m) => m[1]);
    const own = [...line.matchAll(/--export=([a-z0-9_]+)/g)].map((m) => m[1]);
    return [...own, ...refs.flatMap((r) => makefileExportNames(makefile, r))];
}

/** One exported function's type, as the module's own type section states it. */
export interface WasmSig {
    /** Parameter value types, in order: `i32`, `i64`, `f32`, `f64`. */
    readonly params: string[];
    /** Result value types. Empty is a `void` function. */
    readonly results: string[];
}

const VALUE_TYPES: Record<number, string> = { 0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64' };

/**
 * Every function a linked module exports, with its signature.
 *
 * `WebAssembly.Module.exports()` gives the names and the kinds but not the
 * types, so the four sections that carry them are read here: types (1), imports
 * (2, because imported functions occupy the low function indices), functions
 * (3, index -> type) and exports (7). Nothing else in the binary is touched -
 * an unknown or custom section is skipped by its declared size.
 */
export function wasmExportSignatures(bytes: Uint8Array): Map<string, WasmSig> {
    if (bytes.length < 8 || bytes[0] !== 0x00 || bytes[1] !== 0x61 || bytes[2] !== 0x73 || bytes[3] !== 0x6d) {
        throw new Error('not a wasm module: bad magic');
    }
    let o = 8;
    const uleb = (): number => {
        let r = 0, shift = 0, b = 0;
        do { b = bytes[o++]; r |= (b & 0x7f) << shift; shift += 7; } while (b & 0x80);
        return r >>> 0;
    };
    const valueTypes = (n: number): string[] => {
        const out: string[] = [];
        for (let i = 0; i < n; i++) {
            const t = bytes[o++];
            const name = VALUE_TYPES[t];
            if (!name) throw new Error(`unsupported wasm value type 0x${t.toString(16)}`);
            out.push(name);
        }
        return out;
    };

    const types: WasmSig[] = [];
    const funcTypeIdx: number[] = [];
    const exported: { name: string; kind: number; index: number }[] = [];
    let importedFunctions = 0;

    while (o < bytes.length) {
        const id = bytes[o++];
        const size = uleb();
        const end = o + size;
        if (id === 1) {
            for (let n = uleb(), i = 0; i < n; i++) {
                o++;                                        // the 0x60 func form
                const params = valueTypes(uleb());
                const results = valueTypes(uleb());
                types.push({ params, results });
            }
        } else if (id === 2) {
            // Only the function imports matter, but every import must be walked
            // past, and each kind's descriptor has its own shape.
            for (let n = uleb(), i = 0; i < n; i++) {
                // `o += uleb()` would be wrong: JS reads the left `o` before
                // calling uleb(), so the bytes the length itself occupies would
                // be skipped twice over.
                const modLen = uleb(); o += modLen;
                const fieldLen = uleb(); o += fieldLen;
                const kind = bytes[o++];
                if (kind === 0) { uleb(); importedFunctions++; }        // typeidx
                else if (kind === 1) { o++; const lim = bytes[o++]; uleb(); if (lim) uleb(); }   // table
                else if (kind === 2) { const lim = bytes[o++]; uleb(); if (lim) uleb(); }        // memory
                else if (kind === 3) { o += 2; }                                                 // global
                else throw new Error(`unsupported wasm import kind ${kind}`);
            }
        } else if (id === 3) {
            for (let n = uleb(), i = 0; i < n; i++) funcTypeIdx.push(uleb());
        } else if (id === 7) {
            for (let n = uleb(), i = 0; i < n; i++) {
                const len = uleb();
                const name = Buffer.from(bytes.subarray(o, o + len)).toString('utf8');
                o += len;
                exported.push({ name, kind: bytes[o++], index: uleb() });
            }
        }
        o = end;
    }

    const out = new Map<string, WasmSig>();
    for (const e of exported) {
        if (e.kind !== 0) continue;                         // memory, table, global
        const defined = e.index - importedFunctions;
        if (defined < 0) continue;                          // a re-exported import
        const sig = types[funcTypeIdx[defined]];
        if (!sig) throw new Error(`export ${e.name} names function ${e.index}, which has no type`);
        out.set(e.name, sig);
    }
    return out;
}

/** One `wasm_*` member of a binding interface, as TypeScript declares it. */
export interface BoundMember {
    readonly name: string;
    /** `?` on the member: the host tolerates a module without this export. */
    readonly optional: boolean;
    readonly paramTypes: string[];
    readonly returnType: string;
    readonly line: number;
}

/** One TypeScript interface that declares a slice of a module's export table. */
export interface Binder {
    /** `<path>#<InterfaceName>`, repo-relative - the key BINDER_MODULES uses. */
    readonly key: string;
    readonly file: string;
    readonly members: BoundMember[];
}

/** The kernel modules a binder can be bound to. */
export type ModuleId = 'web' | 'bots-test' | 'oracle' | 'oracle-mt' | 'msg';

/**
 * The hand-typed interfaces that still declare a kernel entry point, and the
 * module each binds. A closed list: a new binder is the generated interface of
 * its link, not a line here.
 */
export const BINDER_MODULES: Readonly<Record<string, ModuleId>> = {
};

// Only directories that hold no hand-written TypeScript: dependencies, build
// outputs, and the structgen modules (which emit accessors over a struct, never
// a call into one). Everything a person could write a binder in is walked.
const SKIP_DIRS = new Set(['node_modules', '.next', 'build', 'gen', '.v8cov']);

function tsFiles(dir: string, out: string[] = []): string[] {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        if (entry.name.startsWith('.') || SKIP_DIRS.has(entry.name)) continue;
        const p = join(dir, entry.name);
        if (entry.isDirectory()) { tsFiles(p, out); continue; }
        if (/\.tsx?$/.test(entry.name) && !entry.name.endsWith('.d.ts')) out.push(p);
    }
    return out;
}

/**
 * Every `wasm_*` name the hand-written TypeScript calls on an instance
 * (`ex.wasm_foo(`, `ex.wasm_foo?.(`). A declaration is not a call: the
 * generated interfaces declare every export of every link.
 */
export function calledExports(root: string): Set<string> {
    const out = new Set<string>();
    for (const path of tsFiles(root)) {
        const src = readFileSync(path, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
        for (const m of src.matchAll(/\.(wasm_[a-z0-9_]+)(?=\s*[!?]?\.?\()/g)) out.add(m[1]);
    }
    return out;
}

/** Every hand-written interface or object type alias in the tree that declares a `wasm_*` member, found by shape. */
export function wasmBinders(root: string): Binder[] {
    const binders: Binder[] = [];
    for (const path of tsFiles(root)) {
        const src = readFileSync(path, 'utf8');
        if (!src.includes('wasm_')) continue;
        binders.push(...bindersIn(src, path.slice(root.length).replace(/^\//, '')));
    }
    return binders.sort((a, b) => (a.key < b.key ? -1 : 1));
}

/**
 * The binders one file's text declares. Split out of the walk above so a file
 * the walk deliberately skips - a GENERATED interface under sdk/ts/gen - can be
 * read by the same rules as a hand-written one (e2e/wasm_exports.test.ts).
 */
export function bindersIn(src: string, rel: string): Binder[] {
    const binders: Binder[] = [];
    for (const head of src.matchAll(/(?:^|\n)\s*(?:export\s+)?(?:interface\s+(\w+)[^{=]*|type\s+(\w+)\s*=\s*)\{/g)) {
        // Brace matching rather than a lazy `}` : a member's type can hold
        // braces of its own, and stopping at the first one would read half
        // an interface and call the rest absent.
        const open = head.index! + head[0].length;
        let depth = 1, i = open;
        for (; i < src.length && depth > 0; i++) {
            if (src[i] === '{') depth++;
            else if (src[i] === '}') depth--;
        }
        // Comments blanked rather than cut, so the offsets a member reports
        // as its line still land where the member is. A doc comment naming
        // a RETIRED entry point is otherwise read as a declaration of it.
        const body = src.slice(open, i - 1)
            .replace(/\/\*[\s\S]*?\*\//g, (c) => c.replace(/[^\n]/g, ' '))
            .replace(/\/\/[^\n]*/g, (c) => ' '.repeat(c.length));
        const members: BoundMember[] = [];
        // Not anchored to the start of a line: tools/structgen/test/bench.ts
        // declares five members on two lines, and anchoring read one of
        // each - an under-read that looks exactly like agreement.
        // The lookbehind keeps `__wasm_init_tls` - wasm-ld's own TLS entry,
        // which oracleMtWorker.ts binds - from being read as a kernel
        // `wasm_init_tls` that no module exports.
        // The return type is taken as the first identifier after the colon
        // rather than "everything up to the semicolon": a member written
        // without its trailing `;` (legal inside a `type X = { … }`) was
        // read as no member at all, which is a scanner that agrees with
        // anything.
        for (const m of body.matchAll(/(?<![A-Za-z0-9_])(wasm_[a-z0-9_]+)(\??)\s*\(([^)]*)\)\s*:\s*([A-Za-z0-9_]+)/g)) {
            const args = m[3].trim();
            members.push({
                name: m[1],
                optional: m[2] === '?',
                paramTypes: args ? args.split(',').map((a) => a.split(':').slice(1).join(':').trim()) : [],
                returnType: m[4].trim(),
                line: src.slice(0, open + m.index!).split('\n').length,
            });
        }
        if (members.length) binders.push({ key: `${rel}#${head[1] ?? head[2]}`, file: rel, members });
    }
    return binders;
}

/**
 * How a binder's declarations disagree with a module's export table, one
 * human-readable line per disagreement. Empty is agreement.
 *
 * An OPTIONAL member is allowed to be absent - `wasm_msg_*` is declared `?` in
 * sdk/ts/wasm/bots.ts because the same wrapper types msg.wasm, which does not
 * carry it - but when it is present it must still match.
 */
export function binderMismatches(binder: Binder, module: ModuleId, sigs: Map<string, WasmSig>): string[] {
    const out: string[] = [];
    for (const m of binder.members) {
        const where = `${binder.file}:${m.line} ${m.name}`;
        const sig = sigs.get(m.name);
        if (!sig) {
            if (!m.optional) out.push(`${where}: declared, but ${module}.wasm does not export it`);
            continue;
        }
        if (m.paramTypes.length !== sig.params.length) {
            out.push(`${where}: declared with ${m.paramTypes.length} parameter(s), ${module}.wasm takes ${sig.params.length} (${sig.params.join(', ') || 'none'})`);
        }
        // i64 is the one value type a `number` cannot carry: JS sees a BigInt.
        // Nothing in the kernel exports one today, and if something starts to,
        // the declaration must say `bigint` rather than be silently wrong.
        for (let i = 0; i < Math.min(m.paramTypes.length, sig.params.length); i++) {
            const want = sig.params[i] === 'i64' ? 'bigint' : 'number';
            if (m.paramTypes[i] !== want) out.push(`${where}: parameter ${i + 1} is declared ${m.paramTypes[i]}, ${module}.wasm takes ${sig.params[i]} (${want})`);
        }
        const wantReturn = sig.results.length === 0 ? 'void' : sig.results[0] === 'i64' ? 'bigint' : 'number';
        if (m.returnType !== wantReturn) {
            out.push(sig.results.length === 0
                ? `${where}: declared to return ${m.returnType}, ${module}.wasm returns nothing`
                : `${where}: declared to return ${m.returnType}, ${module}.wasm returns ${sig.results[0]} - a ${wantReturn} the caller must read`);
        }
    }
    return out;
}
