# WASM Anatomy

An interactive, single-file HTML dissection of the three WebAssembly modules the
cnitro rules kernel ships:

| module        | ships as                          | contents |
| ------------- | --------------------------------- | -------- |
| `rules.wasm`  | base64 in `rules_wasm.ts`         | engine + legal-move generator + replay codec |
| `guards.wasm` | base64 in `guards_wasm.ts`        | `game.c` only — browser UI move-gates |
| `bots.wasm`   | gzip static asset `bots.wasm.gz`  | rules **+** every algorithmic bot strategy |

The rendered page lives at [`docs/wasm-anatomy.html`](../../../docs/wasm-anatomy.html) —
open it in any current browser (self-contained, ~0.9 MB, no network).

## What it shows

Per module, seven views:

- **Overview** — size / gzip / function / memory headline, subsystem weight split.
- **Size layout** — a proportional section treemap, the full section table, and
  CODE bytes attributed back to each `.c` source file.
- **Memory layout** — the linear memory as a scrollable address-gutter ribbon:
  the shadow stack (`--stack-first`, grows down), static data, the big zero-init
  BSS buffers, and the 2 MiB replay scratch — anchored to real addresses read
  from the `wasm_*_ptr` getter constants. Proportional / log-scale toggle.
- **Annotated assembly** — every function body, disassembled instruction by
  instruction in fixed columns (offset │ raw bytes │ instruction). Mnemonics are
  colored by class; call targets, `__stack_pointer`, and memory offsets are
  resolved in the trailing annotation. Expand any function, or the whole module.
- **Opcode census** — the instruction mix by class and every distinct opcode by
  frequency.
- **Imports · exports** — the export allow-list, with address getters resolved
  to their linear-memory constants; globals and the indirect-call table.
- **Data segments** — an annotated hex+ASCII dump of the only bytes materialized
  into memory at load (for `bots.wasm`, the bot env-flag key strings).

## Accuracy

The bytes analyzed are **byte-identical to the shipped artifacts** — the build
verifies `build/rules.wasm` and `build/bots.wasm` reproduce the committed
`rules_wasm.ts` / `bots.wasm.gz` exactly. Function *names* are recovered from a
name-preserving companion build (the same link, minus `-Wl,--strip-all`), whose
CODE section is identical, so names map 1:1 onto the shipped bytes. The
disassembler is self-contained (MVP + sign-extension + bulk-memory; the modules
use no SIMD) and validated: every function decodes contiguously and terminates
in `end`.

## Regenerate

```bash
cnitro/tools/wasm_anatomy/generate.sh          # -> docs/wasm-anatomy.html
cnitro/tools/wasm_anatomy/generate.sh out.html # custom output path
```

Requires `clang` (wasm32 target), `wasm-ld`, `llvm-nm`, and `node`. Run it after
any change to the C sources or Makefile WASM flags to refresh the page.

Files:
- `generate.sh` — end-to-end pipeline (build → name build → symbol map → analyze → render)
- `analyze.mjs` — WASM section parser + disassembler → per-module JSON
- `build_html.mjs` — derives the memory map & source attribution, emits the HTML (with the CSS)
- `app.js` — the browser app (all seven views)
