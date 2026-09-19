// Which seat is this device, given a bubble's roster?
//
// PORTED FROM THE FORK UNCHANGED IN BEHAVIOUR (msg_wire.c's msg_seat_resolve*
// family, docs/IMESSAGE_SEAT_IDENTITY_V2.md). Nothing in it was ever about
// cards: it is roster arithmetic over (seat, name) rows plus what Messages will
// tell an extension about who sent the bubble it is looking at. It is pulled out
// into its own file here rather than left inside the envelope, because it is the
// clearest single example of a rule that should never have been game-specific.
//
// WHY IT IS IN C AT ALL. This used to be reassembled at the Swift call site out
// of three smaller calls joined by a `?:` and a `??`, which is this file minus
// its last step, in a language that is not allowed to hold a rule. A device that
// resolves its own seat wrongly plays somebody else's turn.
#ifndef WW_SEAT_H
#define WW_SEAT_H

#include "ww_wire.h"

// Is this name already on the roster?
int ww_name_taken(const WwJoin *joins, int n, const char *name, int name_len);

// The seat carrying this claim name, or -1.
int ww_seat_claimed_by_name(const WwJoin *joins, int n, const char *name, int name_len);

// Does this roster list `cached_seat` under SOMEBODY ELSE'S name? Then the
// numeric cache lost a race and must not be trusted. A roster that names nobody
// at that seat is not evidence either way, and stays permissive.
int ww_seat_cache_disowned(const WwJoin *joins, int n, int cached_seat,
                           const char *name, int name_len);

// The nameless fallbacks, in the order they have to be asked:
//   1. a cached seat in range is the answer;
//   2. if MESSAGES says this bubble came from this device, the last actor was me;
//   3. in a 1:1 chat of two, the receiver is the seat the last actor is not.
// -1 when none of them can answer.
int ww_seat_resolve(int cached_seat, int sender_is_local, int n_players,
                    int last_actor_seat, int chat_is_dm);

// THE seat decision for a live board. No membership check: a live chain carries
// every seated player forward, so a roster that does not list the resolved seat
// is not evidence the seat is not mine - it is a roster this device has not been
// sealed into YET, which is the ordinary state of a two-player DM receiver
// before their first move. Requiring membership here answers "ambiguous" to a
// player the numbers identify exactly, and ambiguous means the spectator board:
// a seated human locked out of their own game.
int ww_seat_resolve_on_board(const WwJoin *joins, int n_joins,
                             int cached_seat, int sender_is_local, int n_players,
                             int last_actor_seat, int chat_is_dm,
                             const char *name, int name_len);

// The same decision in a LOBBY, WITH the membership check. A lobby bubble is a
// snapshot of who had joined when it was sealed, so a seat it does not list is a
// seat that had not been claimed on this branch - and granting Start off it is
// how a stale invite hands the game to somebody the lobby does not contain.
int ww_seat_resolve_in_lobby(const WwJoin *joins, int n_joins,
                             int cached_seat, int sender_is_local, int n_players,
                             int last_actor_seat, int chat_is_dm,
                             const char *name, int name_len);

#endif
