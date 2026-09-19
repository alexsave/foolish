// The Swift-visible C bridge. `import CWerewolf` and call these directly
// (sdk/swift/Kernel.swift). Built into ios/vendor/Werewolf.xcframework by
// `make -C c ios-lib`.
//
// ONE RESIDENT GAME, one resident envelope. An iMessage extension shows exactly
// one bubble at a time and is memory-capped far below an app, so the bridge owns
// the state and Swift holds nothing but ints and byte buffers. The fork learned
// this the hard way and its rule carries over verbatim: never seal or read
// across an `await` - decoding IS adopting.
//
// NO STRUCTS CROSS THIS BOUNDARY, and no blob either. Every view question is a
// flat accessor returning an int or filling a byte buffer, so there is no layout
// for a Swift decoder to get wrong and no second implementation to drift.
//
// AND EVERY VIEW ACCESSOR READS THE MASKED BLOB. wwi_view_* below do not consult
// the resident WwGame: they build ww_view_put's output for the asked-for viewer
// and read the field out of THAT. So the bridge is a reader of the one masking
// implementation rather than a second one, and a leak cannot be introduced here
// without first introducing it in ww_view.c - where the tests are.
#ifndef WW_API_H
#define WW_API_H

#include <stdint.h>

// Return codes are the kernel's own (WW_OK / negative WW_E*, WW_MSG_EOK /
// negative WW_MSG_E*), passed through unchanged. Swift switches on them.

// ------------------------------------------------------------ the session ---

// Deal a fresh game into the resident slot. `seed` is 32 bytes.
int wwi_new_game(const uint8_t *seed, int n_players);

// Decode AND replay a bubble into the resident slot. The resident game is
// replaced only if the whole chain replays, so a damaged payload leaves the
// device on the game it was already showing.
int wwi_adopt(const uint8_t *payload, int len);

// Rule P over two payloads' bytes: <0 a is preferred, >0 b, 0 the same chain.
// A payload that will not decode loses to one that will; two that will not
// decode compare equal, because there is nothing to prefer.
int wwi_prefer(const uint8_t *a, int a_len, const uint8_t *b, int b_len);

// Seal the resident game into `out`. `parent` is the previous bubble's bytes (or
// 0/0 at creation) - the bridge computes parent8 rather than making Swift hash.
// Returns the byte count, or a negative WW_MSG_E*.
int wwi_seal(uint8_t *out, int cap, uint64_t game_id, uint16_t sent_at,
             const uint8_t *parent, int parent_len);

// The roster the next seal will carry. Cleared by wwi_new_game.
int wwi_roster_set(int seat, const uint8_t *name, int name_len);
int wwi_roster_count(void);
int wwi_roster_seat(int i);
int wwi_roster_name(int i, uint8_t *out, int cap);

// Which seat is this device, on the resident envelope's roster? The two answers
// differ, and the difference is a rule - see ww_seat.h.
int wwi_seat_on_board(int cached_seat, int sender_is_local, int last_actor_seat,
                      int chat_is_dm, const uint8_t *name, int name_len);
int wwi_seat_in_lobby(int cached_seat, int sender_is_local, int last_actor_seat,
                      int chat_is_dm, const uint8_t *name, int name_len);
int wwi_name_taken(const uint8_t *name, int name_len);

// --------------------------------------------------------------- the night --

// One night record from `seat`. `carry` non-zero also passes for everyone late
// ahead of this seat in tonight's rotation. `chat` is the wolves' line and is
// refused (WW_ECHAT) from a non-wolf rather than dropped.
int wwi_night_act(int seat, int target, const uint8_t *chat, int chat_len, int carry);

// The day's verdict. WW_NO_SEAT (255) for no lynch.
int wwi_day_lynch(int target);

// Seconds left before Send may be tapped, and whether the carry may be offered.
// Both are kernel rules, not view opinions - see ww_game.h.
int wwi_send_floor_remaining(uint16_t opened_at, uint16_t now);
int wwi_may_carry(uint16_t last_seal_at, uint16_t now);

// ---------------------------------------------------------------- the view --
//
// `viewer` is a seat, or -1 for a spectator. Every one of these reads the masked
// blob for that viewer.

int wwi_view_phase(int viewer);
int wwi_view_night(int viewer);
int wwi_view_n_players(int viewer);
int wwi_view_turn(int viewer);
int wwi_view_winner(int viewer);
int wwi_view_alive(int viewer, int seat);
int wwi_view_my_role(int viewer);

// WW_ROLE_* if this viewer is entitled to know, else 255 (WW_ROLE_UNKNOWN).
int wwi_view_role_of(int viewer, int seat);

// 0 not sent, 1 sent, 2 carried by a later player. THE field a third seat gets,
// and it is the same value whatever the sender's role was.
int wwi_view_sent(int viewer, int seat);

// The viewer's own pick for tonight: the seat, or 255 if they have not picked.
int wwi_view_own_target(int viewer);

// Tonight's deciding wolf, or 255 - which is what every non-wolf is told.
int wwi_view_decider(int viewer);

// The wolves' channel. 0 rows for a non-wolf, and the bytes are simply not there.
int wwi_view_chat_count(int viewer);
int wwi_view_chat_seat(int viewer, int i);
int wwi_view_chat_line(int viewer, int i, uint8_t *out, int cap);

// The seer's answer about `seat`: WW_TEAM_* or 255 for "you were not told".
int wwi_view_reading(int viewer, int seat);

// Who died, per night. 255 for nobody.
int wwi_view_victim(int viewer, int night);
int wwi_view_lynched(int viewer, int night);

#endif
