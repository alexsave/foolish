// ios_bots_api.h — the Swift-visible bot half of the bridge.
//
// A SEPARATE header, module and library from ios_api.h on purpose. The iMessage
// extension plays people, never bots, and it links only the core
// (Foolish.xcframework / module CFoolish). These four entries live in
// FoolishBots.xcframework, whose objects pull in the strategy ladder - 21
// brains, cordite's rollout simulator, the roster and its knob table, about 70%
// of the native kernel by size - so a binary that does not name them does not
// carry them. That is enforced by there being nothing to name: the declarations
// are not in ios_api.h at all.
//
// See c/ios/ios_internal.h for the C-side split this mirrors, and
// c/ios/split_check.sh for the check that keeps it true.
//
// Returns are the same FIO_E* codes ios_api.h documents (negative on failure,
// FIO_ENOGAME = -2 with no resident game). The resident game is the core's -
// there is exactly ONE, reached through fio_resident_game() - so a bot drive
// here acts on the same board fio_state_packed reads.
#ifndef CNITRO_IOS_BOTS_API_H
#define CNITRO_IOS_BOTS_API_H

// Assign a strategy to a seat (offline bots). strategy_id is a FIO strategy id
// (0..fio_strategy_count()-1, see fio_strategy_name). Seat 0 is conventionally
// the local human but nothing enforces that. Safe to call any time before the
// seat is asked to choose. Returns FIO_EOK or a negative error.
int fio_set_seat_strategy(int seat, int strategy_id);

// Drive one bot cycle, result packed: u32 n_actions, per action
// {seat, pace, type, n_cards, cards[], attacks[]}, then i32 stop, ended,
// delayMs (LE). The BotDriveWire Swift decoder reads it.
int fio_bot_drive_packed(int human_mask, char *out, int cap);

// ---------- the offline bot roster (§7.2) ----------------------------------

// Number of exposed offline strategies.
int fio_strategy_count(void);
// Name of strategy `id` (e.g. "espresso"), written to `out`. Bytes written or negative.
int fio_strategy_name(int id, char *out, int cap);

#endif
