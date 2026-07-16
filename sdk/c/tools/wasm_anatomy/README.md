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

## Regenerate (this repo)

```bash
sdk/c/tools/wasm_anatomy/generate.sh          # -> docs/wasm-anatomy.html
sdk/c/tools/wasm_anatomy/generate.sh out.html # custom output path
```

Requires `clang` (wasm32 target), `wasm-ld`, `llvm-nm`, and `node`. Run it after
any change to the C sources or Makefile WASM flags to refresh the page.

## Any wasm (generic)

The parser, disassembler and page are not cnitro-specific — point the generic
driver at any `.wasm` file(s):

```bash
sdk/c/tools/wasm_anatomy/wasm-anatomy.sh app.wasm            # -> wasm-anatomy.html
sdk/c/tools/wasm_anatomy/wasm-anatomy.sh a.wasm b.wasm \
    -o out.html -t "My modules" -s "subtitle"
```

Requires only `node` — no build step, no toolchain. Each module is parsed and
disassembled straight from its bytes; the page degrades gracefully by what the
binary carries:

| the wasm has…              | you get |
| -------------------------- | ------- |
| a `name` custom section    | real function names + grouping by name prefix |
| no names (`--strip-all`)   | functions by index (`func[N]`), one bucket |
| imports (fns / memory / …) | a full imports table; imported-memory limits drive the map |
| exported const getters     | resolved to their value in the exports table |

Source-file attribution and the labeled named-buffer memory map are the only
cnitro-specific extras (they need the object symbol tables and the `wasm_*_ptr`
getters); `generate.sh` supplies them, `wasm-anatomy.sh` omits them.

## Files
- `wasm-anatomy.sh` — generic driver for arbitrary wasm (config → analyze → render)
- `generate.sh` — cnitro pipeline (build → name build → symbol map → config → analyze → render)
- `analyze.mjs` — config-driven WASM section parser + disassembler → per-module JSON
- `build_html.mjs` — derives the memory map & attribution, emits the HTML (with the CSS)
- `app.js` — the browser app (all seven views)

Both drivers build a small `config.json` (`{title, subtitle, symfile?, modules:[{key, human, blurb?, wasm, named?}]}`) and hand it to `analyze.mjs` + `build_html.mjs` — that config is the whole generalization seam.
