#ifndef SNAP_ROOM_H
#define SNAP_ROOM_H

// A snapshot store that is full DROPS the hook: the store's design (MAX_SNAPS,
// game.h). A dropped hook is an animation that skips its tail, never a corrupt
// store, and the table's delta gate then refuses the operation (table.c
// table_cast_delta_ok: a full store is past EVW_DELTA_MAX_SNAPS), so every
// viewer is sent the full cast (table_cast) instead of a delta.
//
// THE LOUD CHECK IS OPT-IN. Built with -DENGINE_SNAP_ASSERT (the kernel's own
// native suites and tools: foolish/c/Makefile), a drop asserts, so a store that
// an operation outgrew (the flip and the opening seats were the first
// casualties of a deal that did) fails a test instead of passing it quietly.
// It used to be on in every hosted build without NDEBUG, and `cbase serve` is
// one: an assert there aborts the whole process, every table down, for what
// the store handles by design. No input reaches it today (table.c
// TABLE_ACTION_HOOKS_MAX is the widest window, and MAX_SNAPS is asserted to
// hold it), and no input may ever abort the server through it.
//
// ITS OWN HEADER, included only by the .c files that own a store (table.c,
// replay_steps.c), and never by a header. structgen parses the layout headers
// for bare triples with no libc (tools/structgen/gen.sh: the iOS and Android
// runs), where only the freestanding headers exist, so a hosted include like
// <assert.h> anywhere in game.h's include graph stops `npm run gen` - and with
// it every web build and test lane - on "'assert.h' file not found".
#if defined(ENGINE_SNAP_ASSERT) && __STDC_HOSTED__ && !defined(NDEBUG)
#include <assert.h>
#define ENGINE_SNAP_ROOM(ok) assert((ok) && "snapshot store full: a hook would be dropped")
#else
#define ENGINE_SNAP_ROOM(ok) ((void)0)
#endif

#endif
