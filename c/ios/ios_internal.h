// ios_internal.h — what the bridge's translation units share with each other.
// NOT part of the Swift-visible contract; that is ios/include/ios_api.h.
//
// The bridge is split so a binary that never drives a bot does not link one.
// `Foolish.xcframework` is a static archive and ld pulls it in per OBJECT, so
// one translation unit that names bot_drive drags the whole strategy ladder
// (21 brains, cordite's simulator, the roster and its knob table - three
// quarters of the native kernel by size) into any binary that uses ANY of the
// bridge. The iMessage extension plays people, never bots, and used to carry
// all of it because ios_api.c held both halves. ios_bots_api.c is now the only
// object that names a bot symbol.
//
// That split needs the two files to share the resident game, which is what this
// header is for. It is an accessor rather than an `extern Game` so the game
// stays owned by one file, stays static, and cannot be reseated behind that
// file's back - and so "is there a game" has one answer rather than two symbols
// that could disagree.
#ifndef CNITRO_IOS_INTERNAL_H
#define CNITRO_IOS_INTERNAL_H

#include "game.h"

// The one resident game, or NULL when fio_new_game has not run. Callers in
// another TU return FIO_ENOGAME on NULL, exactly as ios_api.c's own entries do.
Game *fio_resident_game(void);

#endif
