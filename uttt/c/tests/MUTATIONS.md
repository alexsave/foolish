# Every new test, seen red

Each row is one mutation applied alone, with the file's own copy kept and restored byte for byte, the file touched a second later so make cannot reuse a same-second build, then rebuilt and run.
Only the tests added with the settled-frame cache and the sincos pass are listed here.

## uttt_web_test.c (wasm/uttt_web.c compiled as plain C)

| Test | Mutation | Assertion that went red |
|---|---|---|
| cached frame == rebuilt | `if (cache_ok && ...)` becomes `if (0 && ...)` (never a hit) | `uttt_web_test.c:107` "the repeat call did not rebuild" |
| cached frame == rebuilt | `cache_f = f` not stored | `uttt_web_test.c:107` "the repeat call did not rebuild" |
| walking the clock | the key skips `mark_t` (memcmp from byte 4) | `uttt_web_test.c:142` "walking == rebuilt at this clock" |
| walking the clock | the key skips `line_t` and everything after (`sizeof f - 20`) | `uttt_web_test.c:142` "walking == rebuilt at this clock" |
| inputs invalidate | `uw_seek` stops dropping the cache | `uttt_web_test.c:196` "the next position is drawn, not the cached one" |
| inputs invalidate | `uw_load` stops dropping the cache | `uttt_web_test.c:208` "another drawing byte is another picture", `:210`, `:214` "a refused load drops the cache" |

Mutants that survive, and why they are equivalent:

- `uw_motion` stops dropping the cache: the key holds every field the motion produces at the clock, so a changed plan with the same frame is the same list.
- The key ignores `outline_t`, `outline_a` and `words` (`sizeof f - 12`, `- 8`): the page can only ask for the resting or the "theirs" plan, and neither ever gives an outline, so those fields are constant on the page (a host that composes the outline, the app, does not use this file).

Measured by the same test (arm64 Mac, `-O3 -flto`, the last position of a 40-ply game, 1000 settled `uw_frame` calls): cache on 0.01 us a call, cache off 391 us a call, about 30000x.

## uttt_draw_math_test.c (uttt_pen.c and uttt_draw.c included)

| Test | Mutation | Assertion that went red |
|---|---|---|
| pow75 | `sqrtf(x) * sqrtf(sqrtf(x))` becomes `sqrtf(x) * sqrtf(x)` (the .25 root made .5) | `uttt_draw_math_test.c:44` "within 2 ulp of powf(x, .75)" |
| the cap fan | `CAP_COS[2]` .5f becomes .51f | `:29` "CAP_COS[j] = cos(pi j / 6)", `:31` "a unit vector" |
| the cap fan | `CAP_SIN[5]` .5f becomes .4f | `:30` "CAP_SIN[j] = sin(pi j / 6)", `:31` "a unit vector" |
