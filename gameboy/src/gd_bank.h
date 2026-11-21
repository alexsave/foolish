// gd_bank - where the ROM's code lives, for the entry points that cross banks.
//
// The rules core, the RNG, the input interrupt and GBDK's library sit in the
// fixed bank 0, so code in any bank can call them. The UI is in bank 1; the
// random bot's menu (gd_menu.c) and each bot brain have banks of their own
// (Makefile ROM_BANKS), and are reached through GBDK's banked calls. GD_BANKED
// marks those entry points; it is empty on the Mac and in the 32 KB test ROM.
#ifndef GD_BANK_H
#define GD_BANK_H

#if defined(__SDCC) && defined(GD_BANKED_BUILD)
#include <gbdk/platform.h>
#define GD_BANKED BANKED
#else
#define GD_BANKED
#endif

#endif
