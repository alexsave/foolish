// table_host.c - the kernel's entry surface, natively, for the Node suites.
//
// The TypeScript tests that hold SHIPPED TypeScript (the page in jsdom, the
// web's move gates, its replay screen, the Oracle) need a server's half of a
// game to do it: a board the kernel sealed, the envelope a viewer reads, the
// push after a move, a bot game played to its replay code. The server is
// c_base, which links the kernel natively, so the tests get that half from the
// same native kernel rather than from a whole-kernel wasm link nothing ships
// (bots_test.wasm, retired with this file).
//
// THE SURFACE IS THE WASM ENTRY FILES THEMSELVES. c/wasm/*.c are plain C over
// a static IO buffer and the kernel's resident slots; this file includes them,
// so every entry below is called with its real definition in view (a wrong
// arity does not compile) and behaves byte for byte as it does in web.wasm,
// whose objects are compiled from the same sources under the same caps
// (TABLE_HOST_CFLAGS in c/Makefile is WASM_BOT_CFLAGS for a native target).
//
// THE WIRE (e2e/helpers/table_host.ts is the other end). Two FIFOs, named on
// the command line, request then response. A frame is a u32 length and that
// many bytes, little-endian:
//
//   'C' u8 name_len, name, u8 argc, f64 args[argc]  -> f64 result
//   'R' f64 address, u32 len                        -> len bytes at address
//   'W' f64 address, bytes                          -> nothing
//
// and every answer starts with a status byte: 0 and the result, or 1 and a
// message. An address is the native pointer an entry handed back, carried as a
// double (exact below 2^53, which every user-space address is). Arguments are
// doubles because that is what a JavaScript number is; an integer parameter
// gets it through int64_t, so a negative number reaches an unsigned parameter
// the way wasm's i32 wraps it, and an int result comes back SIGNED through
// int32_t, the way wasm hands an i32 to JavaScript.
//
// The structs a test reads or writes at those addresses are read through the
// generated wasm32 accessors (sdk/ts/gen/game_layout.bots.ts and friends). That
// is sound only because the native layout of every one of them is the wasm32
// layout, and that is checked, not assumed: this build's SG_LAYOUT_HASH is
// structgen's hash of the layouts under THIS compile's flags and triple
// (build/layout/tablehost.hash), and the TypeScript end refuses a host whose
// wasm_layout_hash() is not the LAYOUT_HASH the accessors were generated with.
//
// A test tool: nothing ships it, and it trusts its one client - a read or write
// at an address no entry handed out is the client's bug and may crash the host,
// which the client sees as a closed pipe.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../wasm/wasm_api.c"
#include "../wasm/wasm_bots_api.c"
#include "../wasm/wasm_msg_api.c"
#include "../wasm/wasm_table_api.c"
#include "../wasm/wasm_net_api.c"

static inline int64_t I64(double d) { return (int64_t)d; }

// Every entry a client may call: E(result kind, name, arity, (arguments)).
// Kinds: I an int (signed, as wasm hands it over), P a pointer, D a double,
// V nothing.
//
// NOT offered: an entry that answers "an address, or a negative code" in one
// int ((int)(uintptr_t)&x - wasm_bot_roster_dump, wasm_replay_summary,
// wasm_replay_decoded_next, the wasm_anim_roles_* three), since a native
// address does not fit one; and wasm_setenv_from_io / wasm_clearenv, whose
// table only wasm's getenv reads (wasm_bots_api.c).
#define TABLE_HOST_ENTRIES(E) \
    E(P, wasm_anim_finish_ask_ptr, 0, ()) \
    E(P, wasm_anim_finish_ptr, 0, ()) \
    E(I, wasm_anim_finish_rows, 0, ()) \
    E(P, wasm_anim_frame_ptr, 0, ()) \
    E(P, wasm_anim_hand_ask_ptr, 0, ()) \
    E(I, wasm_anim_hand_laid_out, 0, ()) \
    E(P, wasm_anim_hand_laid_ptr, 0, ()) \
    E(I, wasm_anim_plan_at, 1, (I64(a[0]))) \
    E(P, wasm_anim_plan_ptr, 0, ()) \
    E(P, wasm_anim_reversal_ask_ptr, 0, ()) \
    E(I, wasm_anim_reversal_order, 0, ()) \
    E(P, wasm_anim_reversal_plan_ptr, 0, ()) \
    E(I, wasm_anim_set_transport, 1, (I64(a[0]))) \
    E(I, wasm_anim_should_drop_stale, 4, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]))) \
    E(I, wasm_anim_shown_ledger_allows, 2, (I64(a[0]), I64(a[1]))) \
    E(P, wasm_anim_stale_ask_ptr, 0, ()) \
    E(I, wasm_anim_stale_optimistic, 0, ()) \
    E(P, wasm_anim_stale_release_ptr, 0, ()) \
    E(I, wasm_apply_action, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_ask_refusal_encode, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_attack, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_belief_probe_dump, 0, ()) \
    E(P, wasm_belief_probe_ptr, 0, ()) \
    E(V, wasm_belief_probe_reset, 0, ()) \
    E(I, wasm_bot_cycle_delay_ms, 0, ()) \
    E(I, wasm_bot_drive, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_bot_drive_log_start, 0, ()) \
    E(I, wasm_bot_eligible_mask, 1, (I64(a[0]))) \
    E(I, wasm_can_cover, 5, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]), I64(a[4]))) \
    E(I, wasm_card_list_parse, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(P, wasm_cards_a_ptr, 0, ()) \
    E(P, wasm_cards_b_ptr, 0, ()) \
    E(I, wasm_choose_move, 2, (I64(a[0]), I64(a[1]))) \
    E(V, wasm_clear_logs, 0, ()) \
    E(I, wasm_client_action_request, 3, (I64(a[0]), I64(a[1]), a[2])) \
    E(I, wasm_client_action_response, 1, (I64(a[0]))) \
    E(I, wasm_client_adopt_envelope, 1, (I64(a[0]))) \
    E(I, wasm_client_ask_apply, 0, ()) \
    E(I, wasm_client_ask_encode, 0, ()) \
    E(P, wasm_client_ask_ptr, 0, ()) \
    E(I, wasm_client_board_edit, 0, ()) \
    E(P, wasm_client_board_edit_ptr, 0, ()) \
    E(I, wasm_client_cast_open, 2, (I64(a[0]), I64(a[1]))) \
    E(D, wasm_client_cast_version, 1, (I64(a[0]))) \
    E(P, wasm_client_conflict_ptr, 0, ()) \
    E(I, wasm_client_detail, 0, ()) \
    E(P, wasm_client_edit_view_ptr, 0, ()) \
    E(P, wasm_client_event_ptr, 0, ()) \
    E(P, wasm_client_final_view_ptr, 0, ()) \
    E(P, wasm_client_gesture_ptr, 0, ()) \
    E(I, wasm_client_identity, 0, ()) \
    E(I, wasm_client_identity_at, 0, ()) \
    E(I, wasm_client_identity_begin, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_client_identity_seat, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_client_keep_board, 1, (I64(a[0]))) \
    E(P, wasm_client_move_ptr, 0, ()) \
    E(I, wasm_client_move_wire, 0, ()) \
    E(I, wasm_client_optimistic_apply, 1, (I64(a[0]))) \
    E(I, wasm_client_plan, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_client_plan_step, 2, (I64(a[0]), I64(a[1]))) \
    E(P, wasm_client_plan_step_ptr, 0, ()) \
    E(I, wasm_client_play, 0, ()) \
    E(P, wasm_client_play_ptr, 0, ()) \
    E(I, wasm_client_push_final, 0, ()) \
    E(I, wasm_client_push_next, 0, ()) \
    E(I, wasm_client_push_open, 4, (I64(a[0]), I64(a[1]), I64(a[2]), a[3])) \
    E(I, wasm_client_push_settle, 1, (I64(a[0]))) \
    E(I, wasm_client_rearrange_hand, 1, (I64(a[0]))) \
    E(I, wasm_client_refusal_decode, 1, (I64(a[0]))) \
    E(P, wasm_client_refusal_ptr, 0, ()) \
    E(P, wasm_client_response_ptr, 0, ()) \
    E(I, wasm_client_revert_board, 0, ()) \
    E(P, wasm_client_rules_ptr, 0, ()) \
    E(P, wasm_client_rules_view_ptr, 0, ()) \
    E(P, wasm_client_settle_ptr, 0, ()) \
    E(I, wasm_client_validate, 1, (I64(a[0]))) \
    E(P, wasm_client_view_ptr, 0, ()) \
    E(I, wasm_client_view_rules, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_cover, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_events_serialize, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_events_serialize_from, 4, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]))) \
    E(I, wasm_export_logs, 0, ()) \
    E(I, wasm_export_logs_masked, 0, ()) \
    E(I, wasm_export_logs_masked_from, 1, (I64(a[0]))) \
    E(I, wasm_export_snapshot, 1, (I64(a[0]))) \
    E(I, wasm_export_state, 0, ()) \
    E(I, wasm_finalize_win, 1, (I64(a[0]))) \
    E(P, wasm_fixture_begin, 0, ()) \
    E(I, wasm_fixture_seal, 0, ()) \
    E(I, wasm_fixture_game_bytes, 0, ()) \
    E(I, wasm_fixture_seat, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(P, wasm_fixture_stamp_ptr, 0, ()) \
    E(I, wasm_fixture_title, 1, (I64(a[0]))) \
    E(I, wasm_game_done, 0, ()) \
    E(P, wasm_game_ptr_internal, 0, ()) \
    E(I, wasm_good, 1, (I64(a[0]))) \
    E(I, wasm_hand_layout, 2, (I64(a[0]), a[1])) \
    E(P, wasm_hand_layout_ptr, 0, ()) \
    E(P, wasm_hand_metrics_ptr, 0, ()) \
    E(I, wasm_human_mask, 0, ()) \
    E(V, wasm_import_logs, 0, ()) \
    E(I, wasm_import_state, 2, (I64(a[0]), I64(a[1]))) \
    E(V, wasm_import_strategy_keys, 0, ()) \
    E(V, wasm_init, 0, ()) \
    E(I, wasm_io_cap, 0, ()) \
    E(P, wasm_io_ptr, 0, ()) \
    E(I, wasm_layout_hash, 0, ()) \
    E(I, wasm_legal_moves, 1, (I64(a[0]))) \
    E(P, wasm_legal_moves_ptr, 0, ()) \
    E(P, wasm_moves_ptr_internal, 0, ()) \
    E(I, wasm_msg_rematch, 3, (I64(a[0]), a[1], I64(a[2]))) \
    E(I, wasm_net_do, 1, (a[0])) \
    E(I, wasm_net_due_ms, 1, (a[0])) \
    E(P, wasm_net_error, 0, ()) \
    E(P, wasm_net_event, 1, (I64(a[0]))) \
    E(P, wasm_net_event_payload, 1, (I64(a[0]))) \
    E(P, wasm_net_inbox, 1, (I64(a[0]))) \
    E(P, wasm_net_intent_ptr, 0, ()) \
    E(P, wasm_net_next_done, 0, ()) \
    E(P, wasm_net_next_request, 1, (a[0])) \
    E(P, wasm_net_on_response, 4, (I64(a[0]), I64(a[1]), I64(a[2]), a[3])) \
    E(V, wasm_net_reset, 0, ()) \
    E(P, wasm_net_row, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(P, wasm_net_session_info, 0, ()) \
    E(I, wasm_net_session_load, 1, (I64(a[0]))) \
    E(I, wasm_net_session_save, 0, ()) \
    E(I, wasm_net_stream_closed, 1, (a[0])) \
    E(I, wasm_net_stream_in, 3, (I64(a[0]), I64(a[1]), a[2])) \
    E(P, wasm_net_stream_next, 1, (a[0])) \
    E(V, wasm_net_stream_opened, 1, (a[0])) \
    E(P, wasm_net_stream_request, 0, ()) \
    E(I, wasm_net_stream_sub, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_net_stream_unsub, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_next_player, 1, (I64(a[0]))) \
    E(I, wasm_pass, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_pickup, 1, (I64(a[0]))) \
    E(I, wasm_rearrange_hand, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_refill, 0, ()) \
    E(I, wasm_reject_reason, 0, ()) \
    E(V, wasm_reload_bot_flags, 0, ()) \
    E(I, wasm_replay_b32_decode, 1, (I64(a[0]))) \
    E(I, wasm_replay_b32_encode, 1, (I64(a[0]))) \
    E(I, wasm_replay_decode, 1, (I64(a[0]))) \
    E(I, wasm_replay_decoded_open, 1, (I64(a[0]))) \
    E(I, wasm_replay_encode_v6, 1, (I64(a[0]))) \
    E(I, wasm_replay_error_detail, 0, ()) \
    E(P, wasm_replay_error_ptr, 0, ()) \
    E(I, wasm_replay_events, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(P, wasm_replay_events_index_ptr, 0, ()) \
    E(I, wasm_replay_extras_decode, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_replay_extras_encode, 0, ()) \
    E(P, wasm_replay_extras_ptr, 0, ()) \
    E(I, wasm_replay_io_cap, 0, ()) \
    E(P, wasm_replay_io_ptr, 0, ()) \
    E(I, wasm_replay_link, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_replay_link_parse, 1, (I64(a[0]))) \
    E(I, wasm_replay_step_count, 1, (I64(a[0]))) \
    E(I, wasm_replay_step_logs, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_replay_step_masked_state, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_replay_step_play, 1, (I64(a[0]))) \
    E(I, wasm_reset_to_lobby, 1, (I64(a[0]))) \
    E(I, wasm_roster_decode, 1, (I64(a[0]))) \
    E(I, wasm_roster_encode, 0, ()) \
    E(P, wasm_roster_spec_ptr, 0, ()) \
    E(P, wasm_roster_trailer_ptr, 0, ()) \
    E(I, wasm_roster_trailer_read, 1, (I64(a[0]))) \
    E(I, wasm_roster_trailer_write, 5, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]), I64(a[4]))) \
    E(V, wasm_seed_rng_deterministic, 0, ()) \
    E(V, wasm_set_deal_seed_bytes, 0, ()) \
    E(V, wasm_set_deterministic_deck, 1, (I64(a[0]))) \
    E(V, wasm_set_game_key, 1, (I64(a[0]))) \
    E(V, wasm_set_rng_base, 1, (I64(a[0]))) \
    E(V, wasm_set_seed, 1, (I64(a[0]))) \
    E(V, wasm_set_strategy_seed, 1, (I64(a[0]))) \
    E(V, wasm_set_strategy_seed_deterministic, 0, ()) \
    E(I, wasm_should_act, 1, (I64(a[0]))) \
    E(I, wasm_snap_aux, 1, (I64(a[0]))) \
    E(I, wasm_snap_count, 0, ()) \
    E(I, wasm_snap_tag, 1, (I64(a[0]))) \
    E(I, wasm_start_game, 0, ()) \
    E(I, wasm_state_deserialize, 1, (I64(a[0]))) \
    E(I, wasm_state_format_version, 0, ()) \
    E(I, wasm_state_serialize, 0, ()) \
    E(I, wasm_strategy_seed_probe, 0, ()) \
    E(I, wasm_table_act, 4, (I64(a[0]), I64(a[1]), a[2], a[3])) \
    E(I, wasm_table_action_response, 3, (I64(a[0]), I64(a[1]), a[2])) \
    E(I, wasm_table_add_bot, 4, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]))) \
    E(I, wasm_table_ask, 6, (I64(a[0]), I64(a[1]), I64(a[2]), I64(a[3]), I64(a[4]), I64(a[5]))) \
    E(I, wasm_table_ask_decode, 1, (I64(a[0]))) \
    E(P, wasm_table_ask_ptr, 0, ()) \
    E(I, wasm_table_bot_drive, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_bot_wait_ms, 1, (a[0])) \
    E(I, wasm_table_bots_need_logs, 0, ()) \
    E(I, wasm_table_cast, 3, (I64(a[0]), I64(a[1]), a[2])) \
    E(I, wasm_table_commit_products, 3, (I64(a[0]), a[1], a[2])) \
    E(P, wasm_table_commit_ptr, 0, ()) \
    E(I, wasm_table_continue, 1, (I64(a[0]))) \
    E(I, wasm_table_create, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_detail, 0, ()) \
    E(I, wasm_table_drive_prefs, 0, ()) \
    E(P, wasm_table_drive_ptr, 0, ()) \
    E(P, wasm_table_elo_ptr, 0, ()) \
    E(I, wasm_table_envelope, 3, (I64(a[0]), I64(a[1]), a[2])) \
    E(I, wasm_table_join, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_leave, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_load, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_needs_bots, 0, ()) \
    E(I, wasm_table_push, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_rankings, 0, ()) \
    E(I, wasm_table_rate, 1, (I64(a[0]))) \
    E(I, wasm_table_ready, 1, (I64(a[0]))) \
    E(I, wasm_table_rearrange_hand, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_redact, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_reject, 0, ()) \
    E(I, wasm_table_replay_code, 2, (I64(a[0]), I64(a[1]))) \
    E(I, wasm_table_replay_extras, 1, (I64(a[0]))) \
    E(I, wasm_table_request_decode, 1, (I64(a[0]))) \
    E(P, wasm_table_request_ptr, 0, ()) \
    E(P, wasm_table_roster_ptr, 0, ()) \
    E(I, wasm_table_seat_of, 1, (I64(a[0]))) \
    E(I, wasm_table_set_deal_seed, 1, (I64(a[0]))) \
    E(I, wasm_table_set_session_log, 1, (I64(a[0]))) \
    E(I, wasm_transition, 0, ()) \
    E(I, wasm_unambiguous_cover, 3, (I64(a[0]), I64(a[1]), I64(a[2]))) \
    E(I, wasm_view_serialize, 1, (I64(a[0]))) \
    /* end */

#define RESULT_I(call) return (double)(int32_t)(call)
#define RESULT_P(call) return (double)(uintptr_t)(call)
#define RESULT_D(call) return (double)(call)
#define RESULT_V(call) do { call; return 0; } while (0)
#define THUNK(kind, name, arity, args) \
    static double th_##name(const double *a) { (void)a; RESULT_##kind(name args); }
TABLE_HOST_ENTRIES(THUNK)

typedef struct { const char *name; int arity; double (*fn)(const double *); } Entry;
#define ROW(kind, name, arity, args) { #name, arity, th_##name },
static const Entry ENTRIES[] = { TABLE_HOST_ENTRIES(ROW) };
#define N_ENTRIES ((int)(sizeof ENTRIES / sizeof *ENTRIES))

#define MAX_ARGS 16

static int in_fd = -1, out_fd = -1;

static int read_all(void *p, size_t n) {
    uint8_t *b = p;
    while (n > 0) {
        ssize_t r = read(in_fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

static void write_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t w = write(out_fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) exit(0);   // the client is gone
        b += w; n -= (size_t)w;
    }
}

static void answer(uint8_t status, const void *body, uint32_t n) {
    uint32_t len = n + 1;
    write_all(&len, 4);
    write_all(&status, 1);
    if (n) write_all(body, n);
}

static void refuse(const char *fmt, const char *what) {
    char msg[256];
    int n = snprintf(msg, sizeof msg, fmt, what);
    answer(1, msg, (uint32_t)(n < 0 ? 0 : n >= (int)sizeof msg ? (int)sizeof msg - 1 : n));
}

static uint8_t *frame;
static uint32_t frame_cap;

static void call(const uint8_t *p, uint32_t n) {
    if (n < 2 || (uint32_t)p[0] + 2 > n) { refuse("table_host: a malformed call%s", ""); return; }
    char name[256];
    memcpy(name, p + 1, p[0]);
    name[p[0]] = 0;
    int argc = p[1 + p[0]];
    const uint8_t *args = p + 2 + p[0];
    if (argc > MAX_ARGS || (uint32_t)(2 + p[0] + 8 * argc) != n) { refuse("table_host: %s: malformed arguments", name); return; }
    double a[MAX_ARGS] = { 0 };
    memcpy(a, args, 8 * (size_t)argc);
    for (int i = 0; i < N_ENTRIES; i++) {
        if (strcmp(ENTRIES[i].name, name) != 0) continue;
        if (ENTRIES[i].arity != argc) { refuse("table_host: %s called with the wrong number of arguments", name); return; }
        double r = ENTRIES[i].fn(a);
        answer(0, &r, 8);
        return;
    }
    refuse("table_host: no entry %s", name);
}

int main(int argc, char **argv) {
    // `table_host --layout-hash`: print the layout hash and exit. The client
    // runs this first, synchronously, so a binary that cannot start - or one
    // built for another layout - is an error there, before it blocks opening a
    // FIFO this process would never open.
    if (argc == 2 && strcmp(argv[1], "--layout-hash") == 0) {
        printf("%u\n", (unsigned)wasm_layout_hash());
        return 0;
    }
    if (argc != 3) { fprintf(stderr, "usage: table_host <request fifo> <response fifo> | --layout-hash\n"); return 2; }
    // The client opens the request FIFO for writing and then the response FIFO
    // for reading; opening in the same order here is what lets both opens return.
    in_fd = open(argv[1], O_RDONLY);
    out_fd = in_fd < 0 ? -1 : open(argv[2], O_WRONLY);
    if (in_fd < 0 || out_fd < 0) { perror("table_host: open"); return 1; }
    for (;;) {
        uint32_t n;
        if (read_all(&n, 4) != 0) return 0;   // the client closed: done
        if (n > frame_cap) {
            free(frame);
            frame_cap = n;
            frame = malloc(n);
            if (!frame) { perror("table_host"); return 1; }
        }
        if (n == 0 || read_all(frame, n) != 0) return 0;
        const uint8_t *p = frame + 1;
        uint32_t len = n - 1;
        double addr;
        uint32_t want;
        switch (frame[0]) {
        case 'C': call(p, len); break;
        case 'R':
            if (len != 12) { refuse("table_host: a malformed read%s", ""); break; }
            memcpy(&addr, p, 8);
            memcpy(&want, p + 8, 4);
            answer(0, (const void *)(uintptr_t)addr, want);
            break;
        case 'W':
            if (len < 8) { refuse("table_host: a malformed write%s", ""); break; }
            memcpy(&addr, p, 8);
            memcpy((void *)(uintptr_t)addr, p + 8, len - 8);
            answer(0, NULL, 0);
            break;
        default: refuse("table_host: unknown request%s", "");
        }
    }
}
