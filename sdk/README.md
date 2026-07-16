# `sdk/` — one kernel, one binding per language

Foolish has exactly one game implementation: the C kernel in [`sdk/c/`](./c)
(codename **cnitro**). Everything else is a thin *binding* that lets a host
language call that kernel. This folder is the home for the kernel and its
bindings — one folder per language, each depending on nothing but the kernel
and the shared domain vocabulary.

```
sdk/
  c/      the kernel — the ONE implementation (game rules, bots, replay,
          the solver). Builds two artifacts every binding loads:
            • bots.wasm      (→ sdk/ts/wasm/bots.wasm.gz)  — the TS binding
            • Foolish.xcframework (→ ios/vendor/)          — the Swift binding
  ts/     the TypeScript binding — wasm/ (instances + FFI), wire/ (the packed
          byte layouts), packed marshalling. Loaded by the web app, the
          Supabase edge functions, and the e2e harness.
  swift/  the Swift binding — EngineC.swift (the fio_* FFI) + the Swift
          models/replay/session. Compiled into the FoolishKit framework;
          loads Foolish.xcframework.
```

A binding never contains game logic — the kernel owns that. The C kernel is
the source of truth; a binding that disagrees with it is a bug.

## Why the artifacts live outside `sdk/`

The compiled kernel is *staged* into each host's deploy tree, because each
vendor's packager insists on it:

- `bots.wasm.gz` sits in `sdk/ts/wasm/` next to `wasm_asset.ts`, which finds
  it via `new URL('./bots.wasm.gz', import.meta.url)` — the same co-located
  file the web bundler emits and the edge runtime reads.
- `Foolish.xcframework` is staged into `ios/vendor/` because Xcode links it
  from the app project.

Source lives here once; `sdk/c`'s Makefile writes the built artifacts where
each host reads them (`make wasm-bots`, `make ios-lib`).

## How the server tiers relate

The Supabase server code under `supabase/functions/_shared/` is layered so
the SDK sits beneath the host-neutral game logic, which sits beneath the one
vendor adapter:

```
core (vocabulary)  ←  sdk  ←  common (host-neutral logic)  ←  adapter (Supabase)
```

`core/`, `common/`, and `adapter/` still live under `_shared/` (they are the
*server's* code); the `sdk/` they depend on is this folder. The dependency
DAG and the "no vendor coupling above the adapter" rule are enforced by
`e2e/validation/layering_validation.test.ts` — it fails the build on an
upward import or a stray `jsr:@supabase`/`Deno.env` in core/sdk/common.

## Deploy note (A10)

Because the TS SDK now lives at repo root (not under `supabase/`), the edge
functions import it via `../../../sdk/ts/...` and `config.toml` reads the
`.gz` via a `../`-escaping `static_files` path. Local build/tsc/tests all
pass; whether `supabase functions deploy --use-api` bundles the out-of-tree
graph and honors that path needs one real deploy to confirm. The fix-forward
(stage the `.gz` under `supabase/`) is documented at the call site in
`supabase/config.toml`.
