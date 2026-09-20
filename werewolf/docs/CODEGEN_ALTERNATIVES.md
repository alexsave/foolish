# Codegen alternatives: the Component Model, embind, and why this repo generates its own bindings

This is the evidence behind one paragraph of `docs/ARCHITECTURE_AS_A_PATTERN.md` ("Why not an off-the-shelf binding generator") and of `docs/C_GAME_SHAPE_MIGRATION.md` Phase 11 item 2.
Those documents claim that the WebAssembly Component Model with `jco`, and Emscripten's `embind`, were measured and refused.
Until this document existed the numbers lived in a brief and could not be re-run, which is not a measurement, it is a memory.

The prototype that produces them is `experiments/component-model/`.
It is not part of any build, any test suite or any CI job: it is a standing experiment that a reader can re-run.

## Why it lives at the repo root and not under `tools/`

`e2e/no_ts_game_shape.test.ts` walks every `.ts`, `.mjs` and `.js` file under `server/`, `sdk/`, `src/`, `e2e/`, `scripts/` and `tools/`, and fails any file that touches a wasm instance's memory and assembles a multi-byte value by hand.
The benchmark's whole point is that its baseline does exactly that, because that is what the repo's own hand-written marshalling used to look like.
Putting it under `tools/` would fail that gate or need an entry in a list the test says only ever shrinks.
It sits beside `rust/` and `offlinefun/` instead, the two research trees the repo already keeps outside the product tree.

## What was measured

One WIT interface with a game-shaped record (`experiments/component-model/wit/kernel.wit`): two enums, an `option<u8>`, a `u16`, a `u32`, and four lists, one of them a list of records each holding its own list.
That is the shape of a real board, not a toy, and the list-of-records-with-lists is the case every binding scheme finds expensive.

Two functions, `import-state(game)` and `export-state() -> game`, implemented three ways over one identical C struct (`experiments/component-model/c/kstate.h`, a cut-down `c/src/game.h` at the same caps):

1. **baseline** - the repo's own way: a static IO buffer, a compact byte wire the host writes and C parses, no allocator, no imports, freestanding C at the repo's exact wasm flags.
2. **component** - the same C behind the WIT interface, made a component with `wasm-tools component embed` / `component new`, transpiled to JS by `jco`.
3. **embind** - the same struct behind Emscripten's `embind`, with `emscripten::val` so it takes and returns the same plain JS objects and arrays the other two do.

A round trip is `import-state` then `export-state`, starting from and ending at the same JS value.
Each variant's round trip is asserted to reproduce its input exactly before anything is timed, so the three are doing the same work.

## Tool versions and machine

| | version |
| --- | --- |
| jco | 1.34.0 (and 1.10.2 as a comparison, pinned via the npm alias `jco110`) |
| wit-bindgen | 0.62.0 (`c604ee01c`, 2026-09-10) |
| wasm-tools | 1.259.0 (`7fc33f279`, 2026-09-10) |
| clang | Homebrew clang 22.1.8, targeting `wasm32` |
| Emscripten | 6.0.2-git |
| terser | 5.51.2 |
| Node | v26.8.1 |
| Deno | 2.9.1 (used only to check that it refuses a component) |
| machine | Apple M1, macOS 27.0 (26A428) |

Measured on 2026-09-17.
The machine was not quiet: load average was 3.19 at the start of the timing run and 2.82 at the end, with other agent sessions on the same box.
That inflates all rows together and the ratios between them are what the argument rests on.

## Reproducing

```
cd experiments/component-model
npm ci                                   # pins jco 1.34.0, jco 1.10.2, terser
bash tools.sh                            # downloads the pinned wasm-tools and wit-bindgen releases into .tools/
bash build.sh                            # builds all three variants
bash sizes.sh                            # the size table below
JCO_DIRS=jco-sync,jco-sync-nodebug,jco-1.10-sync node bench.mjs
```

`build.sh` needs a clang that can target `wasm32`, which on this Mac means Homebrew LLVM and not Apple clang (`WASM_CC` overrides the path), plus `wasm-opt` and `brotli` on `PATH`.
The embind variant is built only if `emcc` is on `PATH`, and both the size table and the bench leave its rows out rather than inventing them when it is not.
Nothing under `out/`, `gen/` or `.tools/` is committed.

## Size

Bytes, gzip at level 9 and brotli at quality 11, as `sizes.sh` prints them.

| | raw | gzip -9 | brotli -11 |
| --- | ---: | ---: | ---: |
| core: `baseline.wasm` (repo flags) | 1,405 | 817 | 713 |
| core: `component_core.wasm` (pre-embed) | 1,824 | 1,087 | 967 |
| core: jco's shipped `kernel.core.wasm` | 1,873 | 1,126 | 1,013 |
| component: `kernel.component.wasm` | 3,944 | 1,761 | 1,580 |
| JS: baseline glue, hand-written | 3,570 | 1,103 | 969 |
| JS: baseline glue, terser | 1,924 | **814** | 739 |
| JS: jco 1.34 sync `kernel.js` | 110,850 | 23,190 | 19,654 |
| JS: jco 1.34 sync, terser | 45,426 | **12,556** | 11,218 |
| JS: jco 1.34 sync `--minify --valid-lifting-optimization` | 53,323 | 13,127 | 11,723 |
| JS: jco 1.34 sync `--minify --optimize` | 53,323 | 13,127 | 11,723 |
| JS: jco 1.34 default (async, core inlined as base64) | 106,321 | 24,589 | 20,868 |
| JS: jco 1.34 default `--minify` | 56,565 | 14,898 | 13,213 |
| JS: jco 1.10.2 sync `kernel.js` | 11,188 | 2,620 | 2,236 |
| JS: jco 1.10.2 sync, terser | 4,764 | 1,804 | 1,549 |
| JS: embind `kernel.js` (emcc `-Oz`) | 27,310 | 7,949 | 7,287 |
| core: embind `kernel.wasm` (emcc `-Oz`) | 16,598 | 7,321 | 6,241 |

**The glue ratio is 12,556 against 814 gzipped, about 15 times, for two functions.**
The hand-written side is the marshal and unmarshal pair out of `bench.mjs`, run through the same terser, so the two bold rows are minified the same way.

Three things the table says that are easy to miss:

- `jco --optimize` did not change the JS at all, byte for byte, and moved only the core module (1,873 to 1,848 raw).
  The glue is not what that flag is for.
- jco 1.10.2's glue is 1,804 B gzipped against 1.34's 12,556, a **7x growth in the generator's own output** across minor versions, almost all of it the async-runtime plumbing 1.34 emits whether or not the component is async.
  A generated-glue budget is hostage to the generator's roadmap in a way a generator you own is not.
- The component core is larger than the baseline core (1,824 against 1,405 raw) before any host code exists, because the canonical ABI's lifting, lowering and `cabi_realloc` are code in the guest.

## Latency

Nanoseconds per round trip (`import-state` + `export-state`), N = 200,000 per run, 7 runs, medians, cases interleaved so drift hits all of them equally.

| variant | lists as `number[]` | lists as `Uint8Array` |
| --- | ---: | ---: |
| baseline, hand marshal | **543** | 626 |
| jco 1.34, sync instantiation | **8,053** | 7,353 |
| jco 1.34, sync, with the debug probe stubbed out | 4,895 | 4,444 |
| jco 1.10.2, sync | rejected | 3,034 |
| embind (`emscripten::val`) | 20,790 | 21,215 |

**The component round trip is 11.7 to 14.8 times the hand-written one**, the higher ratio when lists arrive as plain arrays and the lower when they arrive as typed arrays.
**embind is 34 to 38 times the hand-written one**, and about 2.6 times the component's.

Two diagnostics behind those rows:

- **About 3,150 ns per round trip, roughly 39 percent of jco 1.34's cost, is a per-call debug probe.**
  The generated glue calls `_debugLog(...)` throughout the async-task machinery, and `_debugLog` reads `globalThis?.process?.env?.JCO_DEBUG` on every call rather than once at module scope.
  The `nodebug` row is a copy of the glue with the guard's body replaced by `return`, which removes that lookup and the rest-argument array but not the object literals the call sites build.
  It is a diagnostic to locate the cost, not a configuration anyone can ship.
  Even with it gone the component is still 7 to 9 times the baseline.
- **jco 1.10.2 refuses a plain `number[]` for a `list<u8>`** and throws `offset is out of bounds`, so only the typed-array column exists for it.
  1.34 accepts both.
  A host that must feed the generator's preferred value shape is a host doing marshalling again, which is the thing the generator was supposed to remove.

## Mechanical findings

These are the things a reader would otherwise have to rediscover.

1. **Freestanding C works; wasi-sdk is not required.**
   The component was built with the repo's own `WASM_FLAGS` (`--target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all`) minus the kernel's `-D` knobs, using the same Homebrew clang the repo already builds `bots.wasm` with.
   Nothing in the Component Model toolchain demands a WASI sysroot for a component that imports nothing.

2. **A three-symbol stdlib shim is the whole libc dependency.**
   `wit-bindgen`'s generated C calls `realloc`, `free` and `abort`.
   `c/include/stdlib.h` declares them and `c/component_impl.c` backs them with a fixed static bump arena that resets after each import.
   That is small, but it exists only because of point 4.

3. **`wasm-tools component new` on a core module with no WIT section succeeds silently and produces an empty component.**
   Run it on the `--no-object-file` build with the embed step skipped and it exits 0 with a 1,956 B artifact whose world is `world root {}`: no exports, no error, no warning.
   Only `wasm-tools component wit` or an instantiation that finds nothing tells you.
   A pipeline that loses its embed step does not fail, it ships nothing.

4. **Every list argument forces a `cabi_realloc` call in the guest, and there is no way to opt out.**
   The canonical ABI lowers a `game` argument by calling the guest's exported `cabi_realloc` for the record and for each list inside it before `import-state` ever runs.
   Instrumented and counted on the four-player state used for the bench: **9 allocator calls and 139 bytes per `import-state`** (the record, the deck, the battles, the players array, one per player's hand, the elimination list).
   `export-state` makes **zero**, because the guest returns pointers into its own static storage.
   So the direction that must allocate is exactly the direction a host uses most, and a kernel whose stated property is that it has no allocator has to grow one to be a component at all.

5. **jco's synchronous instantiation exists** (`--instantiation sync`), so a component can be instantiated with no `await` anywhere, which the browser path in this repo requires.
   That is a real answer to an objection that used to be fatal, and it is why the comparison above uses the sync output throughout.

6. **No JavaScript runtime runs a component natively.**
   Node v26.8.1 and Deno 2.9.1 both refuse `kernel.component.wasm` at the header: `expected version 01 00 00 00, found 0d 00 01 00`.
   A component always ships as a core module plus generated JS, so the glue row in the size table is not an implementation detail that a future runtime removes for you today.

7. **Correction to an inherited finding: `--strip-all` does not remove the `component-type` custom section.**
   The 2026-09-17 prototype embedded the WIT after linking on the grounds that `-Wl,--strip-all` would otherwise delete the section `wit-bindgen`'s `_component_type.o` carries.
   Re-checked here with clang 22.1.8 and wasm-tools 1.259.0, that does not reproduce: linking `kernel_component_type.o` directly with `--strip-all` keeps `custom "component-type:kernel"` (565 B), and `wasm-tools component new` on the stripped link produces a component with the identical WIT world.
   `--strip-all` drops the name and `producers` sections; the component type survives.
   `build.sh` keeps the embed-after-link route anyway, because a build that does not depend on a linker's strip behaviour is the one worth writing down, and because point 3 makes the silent-failure mode of getting this wrong expensive.

## Emscripten and `embind`

Unlike the 2026-09-17 prototype, embind here is **measured, not reasoned about**, because `emcc` 6.0.2 was available on the machine.
`experiments/component-model/emscripten/embind_impl.cc` binds the same two functions over the same `KGame`.

What the measurement shows:

- **22 imports.**
  The embind module imports 22 functions from its JS environment.
  The baseline module and the component core import **nothing at all**.
  The repo's import-free kernel property, which is what makes one instantiation shape work across a browser, an edge function, Node, a native server and a phone, is simply not available through embind.
- **11.8 times the module.**
  16,598 B raw against 1,405 B, for the same two functions over the same struct, because `emcc` links a libc, `dlmalloc`, `libc++` and `libc++abi` to get there.
- **A JS runtime on top of that.**
  27,310 B of `kernel.js` (7,949 gzipped), which is not a marshalling layer but a runtime: heap views, an exception path, the embind type registry.
- **34 to 38 times the round trip.**

One honesty note on the shape of the comparison.
embind was given `emscripten::val`, which takes and returns plain JS objects and arrays, so all three variants start from and end at the same value.
The faster embind idiom, `value_object` plus `register_vector`, would make the host construct wrapper objects (`new Module.VectorUint8()`) before every call, which is marshalling by hand with extra steps and is not the same comparison.
A `value_object` variant would narrow the latency gap; it would not touch the 22 imports, the linked libc or the C++ requirement, which are the findings the conclusion rests on.

## The conclusion for this repo

Both alternatives lose on the axis this repo actually optimizes, and the axis is not cleverness, it is what the artifact is allowed to contain.

- The kernel imports nothing, and the Component Model requires it to export an allocator that the host calls on every list.
- The kernel has no libc and no allocator, and embind brings both plus a C++ runtime.
- Two functions cost 12.5 KB of generated glue under `jco` against 0.8 KB written by hand, and that ratio is set by the generator's version, not by the interface.
- The generated glue is a second ABI the repo would have to keep in step with a tool's release schedule, for a boundary the repo already describes completely in C headers.

`tools/structgen` reads the C headers that already exist and emits host-side access to the layout clang computed, in TypeScript and Swift, with a layout hash so a mismatched pair refuses to run.
It brings no runtime, no memory model and no ABI of its own.
That is the distinction worth carrying to another project: **the tool you want is a `bindgen`, not an `emcc`.**

What would change the answer.
If a JS runtime instantiated components natively, the glue row would collapse and the size argument would mostly go with it.
The allocator requirement would still stand, and for this repo that one is structural rather than a matter of bytes.

## Provenance

The prototype was first built and measured on 2026-09-17 in a scratch worktree, and every number in this document is from a re-run of that same code on the same day, from a clean `out/` and `gen/`, after the sources were brought into the repo.
The re-run reproduced the original size table byte for byte and the latency table within noise.
Two things here are new rather than inherited: the embind variant, which the original only reasoned about, and finding 7, which corrects the original.
