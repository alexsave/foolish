// game_id.h - THE GAME ID RULE, its one owner: what a game id is, how a new one is drawn,
// and the one check every reader runs on an id it is handed.
//
// A game id is CB_GAME_ID_LEN characters, each one of CB_GAME_ID_ALPHABET, drawn from
// CSPRNG bytes. Why six: the share link FOOLISH.CARDS/XXXXXX is 20 characters, which fits
// the smallest QR code (version 1, error correction M, alphanumeric mode: upper-case
// letters, digits and a few symbols; the lobby's QR upper-cases the link for it). Seven
// characters or more push the code to version 2. That is the owner's decision, and the id
// is not a secret: what slows a script that guesses ids is rate limiting, not the width.
// app/misses.h is that rule: every lookup, join, move or ask by id that misses (an id of the
// wrong shape, no such game, a game the caller does not sit at, a join refused) costs the
// asking address one of 30 a 5-minute window refills, and an address with none left is
// answered about its own games only.
//
// The pair below is the whole rule. Widening the alphabet later (lower-case letters, say,
// which the QR shows upper-cased) is a one-line change to CB_GAME_ID_ALPHABET: the draw,
// the check and the boot conversion (rows_sql.c cb_rows_sql_convert_game_ids) all read it.
// The alphabet must stay inside what the QR's alphanumeric mode carries once upper-cased,
// and must never hold two characters that upper-case to the same one.
//
// Ids of any other shape (the 12-hex ids drawn between 2026-10-17 and this rule) are no
// game's: every reader refuses them (cb_game_id_ok), and the boot of `cbase serve`
// rewrites any the tables still hold to a fresh id of this shape.
#ifndef CB_GAME_ID_H
#define CB_GAME_ID_H

#include <stdbool.h>
#include <stddef.h>

#define CB_GAME_ID_ALPHABET "0123456789abcdef"
#define CB_GAME_ID_LEN      6

// The room a stored or journaled id has (buffers, the journal's codec): more than
// CB_GAME_ID_LEN so a legacy id the tables or a crash's journal segments still hold can be
// read and rewritten at boot. Never a reader's rule: that is cb_game_id_ok.
#define CB_GAME_ID_MAX      16
_Static_assert(CB_GAME_ID_LEN <= CB_GAME_ID_MAX, "a game id fits its buffers");

// Is s[0..n) a game id: exactly CB_GAME_ID_LEN characters of CB_GAME_ID_ALPHABET?
bool cb_game_id_ok(const char *s, size_t n);

// A fresh id from CSPRNG bytes (each character uniform over the alphabet), NUL-terminated.
void cb_game_id_random(char out[CB_GAME_ID_MAX + 1]);

#endif
