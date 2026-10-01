#ifndef SNAP_ROOM_H
#define SNAP_ROOM_H

// A snapshot store that is about to DROP a hook says so, loudly, in a hosted
// debug build: a dropped hook is an animation that silently skips its tail (the
// flip and the opening seats were the first casualties of a deal that outgrew
// the store). Standard assert(), so NDEBUG turns it off like any other. A
// freestanding build (__STDC_HOSTED__ == 0, -ffreestanding) has no assert.h,
// and there the store keeps dropping cleanly - never corrupting - behind
// MAX_SNAPS's compile-time size check (game.h).
//
// ITS OWN HEADER, included only by the .c files that own a store (table.c,
// replay_steps.c), and never by a header. structgen parses the layout headers
// for bare triples with no libc (tools/structgen/gen.sh: the iOS and Android
// runs), where only the freestanding headers exist, so a hosted include like
// <assert.h> anywhere in game.h's include graph stops `npm run gen` - and with
// it every web build and test lane - on "'assert.h' file not found".
#if !__STDC_HOSTED__ || defined(NDEBUG)
#define ENGINE_SNAP_ROOM(ok) ((void)0)
#else
#include <assert.h>
#define ENGINE_SNAP_ROOM(ok) assert((ok) && "snapshot store full: a hook would be dropped")
#endif

#endif
