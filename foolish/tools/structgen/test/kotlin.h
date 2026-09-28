// structgen fixture: the shapes the KOTLIN emitter has of its own.
//
// Its own file rather than another few fields in snap.h, because snap.h is read
// by the TS fixture (verify.test.ts) and the Swift one (swift.sh) as well: a
// field added there moves every offset after it in two generated modules that
// are compared byte for byte.
//
// Two things are here that snap.h cannot show:
//
//   a member named for a KOTLIN keyword. Swift's keywords and Kotlin's are not
//   the same list - `object`, `val` and `fun` are Kotlin's alone - so a name
//   Swift emits bare is one Kotlin has to backtick.
//
//   the integer widths where KOTLIN'S RULE IS NOT SWIFT'S. Swift widens
//   everything under 8 bytes to Int and loses nothing, because its Int is 64
//   bits. Kotlin's is 32, so a uint32 has to widen to Long instead - and that
//   is the difference this fixture exists to pin.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  n_tags;      // count of tags
    int32_t  in;          // a Kotlin hard keyword, an ordinary C member name
    int32_t  object;      // another
    int32_t  is_open;     // camelCases to `isOpen`, which is NOT a keyword
    uint32_t big_u32;     // 4 bytes unsigned: Int would lose the top one
    uint64_t game_id;     // 8 bytes unsigned: ULong, where Swift says UInt64
    int64_t  delta;       // 8 bytes signed: Long
    float    ratio;       // widens to Double, as every float does
    bool     ready;
    uint8_t  tags[4];     // counted: n_tags of them
    char     name[8];     // NUL-terminated
} KtThing;

// A pointer field. The Kotlin emitter refuses one - a ByteBuffer holds bytes and
// no way to follow an address - which is the one thing Swift reads that Kotlin
// will not.
typedef struct {
    const uint8_t *body;
    uint16_t       body_len;
} KtPtr;
