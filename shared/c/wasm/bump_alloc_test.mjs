// Runs bump_alloc_test.c's module(s): instantiate, call bump_alloc_test(),
// and fail on anything but 0, which is the line of the check that failed.
//
//   node bump_alloc_test.mjs build/bump_alloc_test.wasm build/bump_alloc_test_mt.wasm
//
// The checks are in C. This file only knows that the -matomics build imports
// its memory, shared, at the limits the module was linked with.
import { readFileSync } from 'node:fs';

let failed = 0;
for (const path of process.argv.slice(2)) {
    const mod = new WebAssembly.Module(readFileSync(path));
    const imports = WebAssembly.Module.imports(mod).some((i) => i.kind === 'memory')
        ? { env: { memory: new WebAssembly.Memory({ initial: 2, maximum: 16, shared: true }) } }
        : {};
    const line = new WebAssembly.Instance(mod, imports).exports.bump_alloc_test();
    if (line) { failed++; console.error(`FAIL ${path}: bump_alloc_test.c:${line}`); }
    else console.log(`ok ${path}`);
}
if (!process.argv[2]) { console.error('bump_alloc_test.mjs: no module given'); failed++; }
process.exit(failed ? 1 : 0);
