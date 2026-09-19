// ios_api_view.c - THE CLIENT'S BOARD SLOT: a board as one viewer sees it, and
// the animation sequence walked over it (see ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. The slot's statics are this
// file's alone; ios_api_play.c borrows the slot through fio_client
// (ios_internal.h) so a host that has just adopted an envelope can ask for its
// menu in the same breath.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "client_table.h"

// ---------- the client's slot: a board as one viewer sees it ----------------
//
// The same reader the web uses (client_table.h), which is the point: a board
// off the wire is read ONCE, in C, and both hosts copy the result out through
// generated snapshot readers. MaskedView.swift used to walk state_put's bytes
// in Swift beside the C that writes them, and derive the game-over rule of its
// own accord while it was there.
//
// ONE SLOT, like everything else in this bridge: a host adopts, reads the
// snapshot at fio_view_ptr, and is done with it before anything else touches
// the kernel (the Swift side serializes every call onto one actor). It costs
// 4.5 KB of BSS at the iOS caps, which is a Game's logs being somewhere else.
static ClientTable g_client;
static ClientSlot  g_client_slot;
static int         g_client_ready = 0;

ClientTable *fio_client(void) {
    if (!g_client_ready) { client_init(&g_client, &g_client_slot); g_client_ready = 1; }
    return &g_client;
}

const void *fio_view_ptr(void) { return &fio_client()->view; }

int fio_view_detail(void) { return fio_client()->detail; }

int fio_view_of_resident(int viewer) {
    Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    return client_adopt_board(fio_client(), g, viewer);
}

int fio_view_of_state(const uint8_t *buf, int len, int viewer) {
    if (!buf || len < 0) return FIO_EBADARG;
    return client_adopt_state(fio_client(), buf, len, viewer);
}

int fio_view_of_envelope(const uint8_t *buf, int len) {
    if (!buf || len < 0) return FIO_EBADARG;
    return client_adopt_envelope(fio_client(), buf, len);
}

// ---------- one animation sequence, step by step -----------------------------
//
// An evwire sequence (one frame of fio_replay_last_events_packed, its u16
// length already stripped) walked by the kernel's own reader. EvWire.swift used
// to walk it in Swift - the header, each event's seven fixed bytes, its cards,
// its optional target and battle, and the u16 snapshot behind them - beside the
// C that writes those bytes.
//
// The whole sequence is checked at OPEN, every event and every board, so a
// sequence that opens reads to its end. `bytes` must outlive the walk: the
// kernel reads them where they are.
int fio_push_open(const uint8_t *buf, int len) {
    if (!buf || len < 0) return FIO_EBADARG;
    // as2 (a bare sequence), no identity: an animation frame names no one, and
    // its seats' names are the caller's to merge as they always were.
    return client_push_open(fio_client(), buf, len, 0, 0, 0, 0);
}

// The next step into the event at fio_push_event_ptr and its board into the
// view: 1, 0 when every step has been read, or a negative CLIENT_E_*.
int fio_push_next(void) { return client_push_next(fio_client()); }

// The board the sequence COMMITTED (its trailer), into the view, and the walk
// is closed. Not the last event's board: a step with no events still commits
// one, which is exactly the case (a bare good) where a client most needs it.
int fio_push_final(void) { return client_push_final(fio_client()); }

// The step the last fio_push_next read (client_table.h PushEvent).
const void *fio_push_event_ptr(void) { return &fio_client()->event; }
