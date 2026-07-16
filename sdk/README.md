# `sdk/` — one binding per language to the C kernel

The game has exactly one implementation: the C kernel in [`c/`](../c) (codename
**cnitro**) — the whole engine, at the repo root, because it is *the thing*,
not a binding to it. This folder holds the thin language bindings that let each
host call that kernel. A binding contains no game logic; the kernel owns that,
and a binding that disagrees with it is a bug.

```
c/            the kernel — the ONE implementation (rules, bots, replay, solver).
              Builds the artifacts every binding loads:
                • bots.wasm            → sdk/ts/wasm/bots.wasm.gz   (TS binding)
                • Foolish.xcframework  → ios/vendor/                (Swift binding)
sdk/
  ts/         the TypeScript binding — wasm/ (instances + FFI), wire/ (packed
              byte layouts), packed marshalling. Loaded by the web app, the
              Supabase edge functions, and the e2e harness.
  swift/      the Swift binding — EngineC.swift (the fio_* FFI) + models/replay/
              session. Compiled into FoolishKit; loads Foolish.xcframework.
```

## Why the artifacts live outside here

The compiled kernel is *staged* into each host's deploy tree, because each
vendor's packager insists on it — the source lives in `c/` once, and `c/`'s
Makefile writes the built artifact where each host reads it:

- `bots.wasm.gz` sits in `sdk/ts/wasm/` next to `wasm_asset.ts`, found via
  `new URL('./bots.wasm.gz', import.meta.url)` — the same co-located file the
  web bundler emits and the edge runtime reads (`make wasm-bots`).
- `Foolish.xcframework` is staged into `ios/vendor/` for Xcode (`make ios-lib`).

## How the server relates

The server lives under [`server/`](../server): `server/api/` is the
host-neutral surface (the shared `core`/`common` game logic), and
`server/impls/supabase/` is the one concrete backend. Both depend on this
`sdk/`; neither is depended on by it. The dependency direction
`core ← sdk ← common ← adapter` and the "no vendor coupling above the adapter"
rule are enforced by `e2e/validation/layering_validation.test.ts`.
