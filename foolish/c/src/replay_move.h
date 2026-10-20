// replay_move.h - one move of a live game, coded the way the v6 family codes a
// move: an index into the kernel's legal-move menu at the board it was played on
// (game.h: "the v6-family replay codec codes each action as an INDEX into that
// state's legal-move menu"), on the replay codec's own coder (replay.h
// ReplayCoder).
//
// For a host that logs moves as they happen (c_base's move journal), so the
// coded form of a move has one owner, the kernel's menu, and no host invents a
// move format. Unlike a replay code, which is one game cut once from its deal, a
// move here is coded against the board it was played on, which the host holds,
// and a code carries the menu's size, so a stream of moves from many games
// decodes without any board and the board it is applied to later checks it
// (replay_move_wire refuses a menu of another size).
//
// THE CODE OF ONE MOVE, in coder choices:
//   seat     uniform over MAX_PLAYERS
//   menu_n   v6's varint: the menu's size at that board for that seat
//            (legal_moves_into, capped at REPLAY_MOVE_MENU_CAP)
//   index    uniform over menu_n + 1: the move's place in the menu, or menu_n
//            for a move the menu does not list in that exact order (cards
//            ordered differently, a menu cut at the cap), which is followed by
//            its awire bytes (awire.h): a varint length and each byte uniform
//            over 256.
//   menu_n == REPLAY_MOVE_RAW (a live human's move, replay_move_raw): no menu
//            was enumerated, no index is coded, the awire bytes follow at once.
//            The board's check moves to the replay: the seat must have a move
//            there (replay_move_wire), and the host applies the move, which
//            the engine refuses off the board. A few bytes more per move, and
//            no menu enumeration (exponential in a wide attack hand) on a move.
#ifndef CNITRO_REPLAY_MOVE_H
#define CNITRO_REPLAY_MOVE_H

#include "awire.h"
#include "replay.h"
#include "bot_drive.h"

#include <stdint.h>

// The menu a move is looked up in is enumerated up to this many moves, on both
// sides: a move past it is coded whole (the escape).
#define REPLAY_MOVE_MENU_CAP 1024
// menu_n's value for a move coded raw (no menu looked up).
#define REPLAY_MOVE_RAW (REPLAY_MOVE_MENU_CAP + 1)
// The longest awire frame: kind, n, n cards and n attacks.
#define REPLAY_MOVE_WIRE_MAX (2 + 2 * AWIRE_MAX_CARDS)

typedef struct ReplayMove {
    int     seat;
    int     menu_n;    // the menu's size at the board
    int     index;     // < menu_n: the menu's move; == menu_n: the wire below
    int     wire_len;  // the escaped move's awire bytes (index == menu_n only)
    uint8_t wire[REPLAY_MOVE_WIRE_MAX];
} ReplayMove;

// The move `wire` (an awire frame) by `seat` on board `g`, before it is applied.
// 0, or -REPLAY_EINPUT for a malformed frame or seat, -REPLAY_ECAP for no
// scratch.
int replay_move_of(const Game *g, int seat, const uint8_t *wire, int wire_len, ReplayMove *out);

// The move `wire` by `seat`, coded raw (REPLAY_MOVE_RAW): no board, no menu. 0, or
// -REPLAY_EINPUT for a malformed frame or seat.
int replay_move_raw(int seat, const uint8_t *wire, int wire_len, ReplayMove *out);

// Codes m (encode) or reads it into m (decode). 0, or -REPLAY_E* (also left in
// c->err).
int replay_move_code(ReplayCoder *c, ReplayMove *m);

// The awire frame m names on board g: its bytes into wire (cap >=
// REPLAY_MOVE_WIRE_MAX), returning their length; -REPLAY_ENOTINMENU when g's
// menu for m's seat is not menu_n long (the board is not the one the move was
// coded on; for a raw move, when the seat has no move at g), -REPLAY_EINPUT for a bad index
// or wire, -REPLAY_ECAP for no scratch.
int replay_move_wire(const Game *g, const ReplayMove *m, uint8_t *wire, int cap);

// ---- a bot drive's cycle -----------------------------------------------------
//
// A cycle (bot_drive.h) applies up to BOT_DRIVE_MAX_ACTIONS moves, each on the
// board the one before it left. This walks them on `g`, a scratch copy of the
// board the drive started from (it is played on), with the drive's own seeding
// before each move (bot_drive_seed_decision's APPLY phase, from the table's
// rng_base and the session log's length at the drive), so every board is the
// one the drive saw. encode: acts[0..n) in, each move's code into mv. decode:
// mv[0..n) in, the moves they name into acts (seat and move). 0, or
// -REPLAY_E*: a move off its board's menu, or one the engine refuses.
int replay_move_drive_walk(Game *g, uint32_t rng_base, uint32_t log_offset, int n, BotDriveAction *acts,
                           ReplayMove *mv, bool encode);

#endif
