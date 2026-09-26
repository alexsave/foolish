# String tables: Swift dictionary vs C array vs C array + cache

A toy that looks up foolish's real 388 strings (en + ru) from Swift three ways, each ending in a Swift `String`:

- **Swift dictionary**: foolish today, the `[String: String]` tables tools/datagen generates.
- **C array**: UTTT's way, `const char *[]` indexed by the key enum, `String(cString:)` on every lookup.
- **C array + cache**: the C array, with each key converted to a `String` once, on first use (`Sources/Cache.swift`, 15 lines).

Run: `shared/bench/string_tables/run.sh` (after `bash tools/structgen/gen.sh`).

Measured 2026-09-26 on an Apple silicon Mac, `-O -wmo`:

| | Swift dictionary | C array | C array + cache |
|---|---|---|---|
| Cold: first string in a fresh process (median of 25) | 59 us | 3.7 us | 8.0 us |
| Warm: per lookup, 3M lookups | 33-42 ns | 116 ns | 6.5 ns |
| Stripped size, 2 languages | 96 KB | 66 KB | 67 KB |

The dictionary is built in full on first touch, which is the cold cost every Messages extension launch pays.
The plain C array is slow warm only because it decodes and allocates a `String` per call.
The cache wins all three, and saves about 15 KB per language (about 370 KB over 25).
These are microseconds either way; this is recorded, not scheduled.
