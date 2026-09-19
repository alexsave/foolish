// Per-seat masked views. THIS FILE IS THE ROLE HIDING.
//
// The fork this came from computes "you only see your own hand" in the kernel
// rather than in each client (c/src/view.c, now deleted with the cards). The
// reason carries over intact and matters more here: a client that decides for
// itself what to redact is a client that can leak, and in a deduction game a
// single leak is not a cosmetic bug - it is the whole game. So every surface in
// this product renders from ww_view_put and nothing else ever reads WwGame.role
// directly.
//
// WHAT THE CONTRACT IS, exactly, because "mask the roles" is not enough:
//
//   1. A THIRD SEAT'S VIEW OF A NIGHT RECORD IS THE SAME BYTES WHATEVER THE
//      SENDER'S ROLE IS. Not "the role field is blank" - byte-identical. A
//      wolf's record carries a target that kills and a line to the other
//      wolves; a villager's carries a target that does nothing and no line.
//      Both reduce to {seat, sent} here. If the two ever differ by one byte,
//      somebody will find it, and the night is over.
//   2. THE WOLF LINE DOES NOT APPEAR IN A NON-WOLF'S VIEW AT ALL. Not
//      truncated, not zeroed to its length - absent, so there is no length to
//      read either.
//   3. THE SEER'S READINGS ARE THE SEER'S. Nobody else's view carries them, not
//      even as a count.
//   4. A DEAD SEAT'S ROLE IS PUBLIC. That is the genre's own rule and the day
//      phase has nothing to argue about without it.
//
// The view is a snapshot with no history of choices in it. That is deliberate:
// the target a player picked is theirs, and a "who did you dream about" board
// would hand the table a wolf's kill vote the morning after.
#ifndef WW_VIEW_H
#define WW_VIEW_H

#include "ww_game.h"

// Bump on any layout change. A view blob never persists, but it does cross the
// C-to-Swift boundary, and a Swift decoder reading last build's layout at this
// build's offsets is wrong quietly.
#define WW_VIEW_FORMAT_VERSION 1

// `viewer` values below 0, matching the fork's VIEW_* discipline.
#define WW_VIEW_SPECTATOR (-1)

// What a third seat learns about one seat's night record.
#define WW_SENT_NO      0   // has not sent
#define WW_SENT_YES     1   // sent
#define WW_SENT_CARRIED 2   // a later player carried this seat's pass forward

// The largest blob ww_view_put can write, so callers can put one on the stack.
// Derived from the layout below at WW_MAX_PLAYERS, not measured: a buffer sized
// by observation is a buffer that overflows the first time a field grows.
#define WW_VIEW_MAX ( 1 + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 1                     \
                    + 1 + 2 * WW_MAX_PLAYERS                                \
                    + 1 + 2 * WW_MAX_PLAYERS                                \
                    + 1 + 1                                                 \
                    + 1 + 1 + WW_MAX_PLAYERS * (2 + WW_CHAT_MAX)            \
                    + 1 + 2 * WW_MAX_PLAYERS                                \
                    + 1 + 3 * WW_MAX_NIGHTS )

// Serialize what `viewer` is entitled to see. Returns bytes written.
//
// Layout (all little-endian; every count is the byte that precedes its rows):
//
//   u8  WW_VIEW_FORMAT_VERSION
//   u8  phase
//   u8  n_players
//   u8  night
//   u16 alive mask
//   u16 turn
//   u8  winner
//   u8  my_seat            (WW_NO_SEAT for a spectator)
//   u8  my_role            (WW_ROLE_UNKNOWN for a spectator)
//   u8  n_roles            roles this viewer is entitled to
//       n x { u8 seat, u8 role }
//   u8  n_rows             one per seat
//       n x { u8 seat, u8 sent }      <- contract 1 lives here
//   u8  have_own_choice
//   u8  own_target
//   u8  decider_seat       (WW_NO_SEAT unless the viewer is a living wolf)
//   u8  n_chat             (0 unless the viewer is a living wolf)
//       n x { u8 seat, u8 len, len bytes }
//   u8  n_readings         (0 unless the viewer is the living seer)
//       n x { u8 seat, u8 team }
//   u8  n_history
//       n x { u8 night, u8 victim, u8 lynched }
int ww_view_put(const WwGame *g, int viewer, unsigned char *out);

// The exact length ww_view_put will write, without writing it. Lets a bridge
// size a buffer for the real view rather than for WW_VIEW_MAX, which at ten
// players and three wolves is 4x the truth.
int ww_view_measure(const WwGame *g, int viewer);

#endif
