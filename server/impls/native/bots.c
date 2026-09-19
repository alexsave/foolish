// The per-game bot trampoline - see bots.h.
#define _GNU_SOURCE
#include "bots.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bot_drive.h"
#include "bot_roster.h"
#include "game.h"
#include "push.h"       // epoll_notify_game_changed: the one cross-thread seam out of this thread
#include "srvthread.h"
#include "strategy.h"
#include "view.h"

atomic_ulong g_bot_decisions = 0;
atomic_ulong g_octogen_decisions = 0;
static int g_octogen_strat = -1;

void bots_resolve_octogen_strat(void) {
    // Via bot_roster_at(...)->strat, NOT bot_roster_find's own return value
    // directly - that's a roster ARRAY INDEX, a different number from the
    // STRAT_* id seats actually carry (see lobby.c's add-bot branch for the
    // full story; this counter needs to compare against the SAME value
    // strategy_key holds). -1 (an unknown key, or a somehow-absent roster
    // entry) just means the per-strategy octogen counter never increments -
    // g_bot_decisions still counts every bot's applied actions regardless.
    int ridx = bot_roster_find("octogen");
    const BotRosterEntry *e = ridx >= 0 ? bot_roster_at(ridx) : NULL;
    g_octogen_strat = e ? e->strat : -1;
}

// Per-decision seeding (bot_drive.h bot_drive_seed_decision), the Table's policy:
// the strategy and draw streams are seeded from the board in front of each
// decision and the game's own base, so a bot's move is a function of its game and
// never of what this thread ran before (the Monte-Carlo rollouts of robusta,
// firecracker and gunpowder, cordite's and octogen's worlds and the random bot all
// read those streams). The hook is process-wide and installed once in main,
// before any thread starts; the base is per thread, set by bot_cycle.
static _Thread_local uint32_t t_bot_rng_base;
static void bot_seed_hook(const Game *g, int seat, int phase) {
    (void)seat;
    // Log offset 0: this server keeps one resident Game per slot, so g->logs IS
    // the whole session and g->num_logs is the progress term by itself.
    bot_drive_seed_decision(g, t_bot_rng_base, 0u, phase);
}

void bots_install_seed_hook(void) {
    bot_drive_pre_action_hook = bot_seed_hook;
}

// One bot cycle of `g`: bot_drive under this host's per-decision seeding.
static void bot_cycle(Game *g, uint32_t rng_base, BotDriveOut *drv) {
    t_bot_rng_base = rng_base;
    bot_drive(g, game_human_mask(g), BOT_DRIVE_MAX_ACTIONS, 0, 0, drv);
}

// The bot game-loop, one thread per game - a TRAMPOLINE, not a blocking hook.
// Each pass drives exactly ONE kernel cycle and RETURNS from the kernel; the
// server then decides how to wait. Same split as supabase (which `await`s a
// setTimeout) and the phone (Task.sleep): the KERNEL owns the cycle and the
// delay value (bot_drive + bot_pacing_ms); the host owns how it waits. The
// `Game` struct IS the continuation, so "resume" is just the next bot_drive.
//
// The lock is `s->lock` - this game's own, not a process-wide one (T2a) - held
// while touching the game and RELEASED during the pacing sleep, so bots think
// + throw in over time while other requests for OTHER games (and, thanks to
// the per-game lock, even other requests for THIS game between cycles) keep
// serving. When no bot can act (a human is owed) the thread waits on `cond`
// until /action or /ws signals it - pairing the wait with `s->lock`, same as
// every other access to this slot.
static void *bot_thread(void *arg) {
    thread_disable_cancellation();
    GameSlot *s = arg;
    pthread_mutex_lock(&s->lock);
    while (s->used && s->game.status == GAME_STATUS_PLAYING) {
        uint32_t hmask = game_human_mask(&s->game);   // pure per-Game field read
        BotDriveOut drv;
        // No g_kernel_lock (Stage 5, see "Locking" in registry.h): bot_drive's
        // scratch state is now _Thread_local, so this game's bot_thread
        // never shares it with another game's - s->lock (already held for
        // this whole cycle) is the only serialization this needs.
        bot_cycle(&s->game, s->rng_base, &drv);   // ONE cycle, then returns
        // A bot's move (or the game ending) changes the board exactly like a
        // human's /action does - the /ws state cache must not stay stale
        // just because no HTTP handler touched this slot this time.
        if (drv.n > 0 || drv.ended >= 0) {
            s->version++; game_mark_dirty(s);
            // Stage 6: the one cross-thread epoll seam - see
            // epoll_notify_game_changed's own doc (push.h) for why this
            // specific call site is it.
            epoll_notify_game_changed(s);
        }
        // Stage 4 instrumentation (see g_bot_decisions/g_octogen_decisions'
        // doc in bots.h): count every action this cycle actually applied, and -
        // since drv.actions[] names the acting seat - how many of those
        // were an "octogen" seat specifically. Read under s->lock, which we
        // already hold for the whole cycle, so s->game.players[] is stable.
        if (drv.n > 0) {
            atomic_fetch_add_explicit(&g_bot_decisions, (unsigned long)drv.n, memory_order_relaxed);
            if (g_octogen_strat >= 0) {
                unsigned long oct = 0;
                for (int i = 0; i < drv.n; i++) {
                    int aseat = drv.actions[i].seat;
                    if (aseat >= 0 && aseat < s->game.num_players &&
                        s->game.players[aseat].strategy_key == g_octogen_strat)
                        oct++;
                }
                if (oct > 0) atomic_fetch_add_explicit(&g_octogen_decisions, oct, memory_order_relaxed);
            }
        }

        if (drv.ended >= 0) break;   // the kernel already flipped g->status to GAME_OVER
        if (drv.stop == BOT_STOP_NO_ELIGIBLE) {              // a human's move is owed
            pthread_cond_wait(&s->cond, &s->lock);            // sleep until /action or /ws wakes us
            continue;
        }

        // A visible cycle landed - the kernel prices the wait; the host owns the
        // loop and the sleep (the trampoline). Lock released while we wait.
        int delay = bot_cycle_delay_ms(&s->game, hmask, &drv);
        if (delay > 0) {
            pthread_mutex_unlock(&s->lock);
            usleep((useconds_t)delay * 1000);
            pthread_mutex_lock(&s->lock);
        }
    }
    s->bot_running = false;
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

void start_bot_loop(GameSlot *s) {
    if (s->bot_running) return;
    // No bot seats -> no bot thread. An all-human game would otherwise spawn a
    // bot_thread that immediately blocks on `cond` forever with nothing to
    // drive. That's normally harmless, but under fast game churn (push-only
    // completes games in a fraction of a second, so a load run does hundreds of
    // /meta-start rematches) the per-spawn cost dominates: creating a thread
    // zeroes its whole static-TLS block, and THIS build's TLS is large - it
    // embeds the 64 KiB _Thread_local scratch buffers in worker_push_stale and
    // ws_send_frame. That TLS memset (_dl_allocate_tls) was ~18% of all
    // instructions in the assembly profile (PROFILE_HOTPATH.md). Skipping the
    // spawn when every seat is human removes it entirely; real games with a bot
    // still get exactly one bot_thread for their (long) lifetime.
    uint32_t human = game_human_mask(&s->game);
    uint32_t all   = (s->game.num_players >= 32) ? 0xffffffffu
                                                 : ((1u << s->game.num_players) - 1u);
    if ((human & all) == all) return;   // every seated player is human - nothing to drive
    s->bot_running = true;
    pthread_t t;
    if (pthread_create(&t, NULL, bot_thread, s) == 0) pthread_detach(t);
    else s->bot_running = false;
}

int run_bot_seeding_self_test(void) {
    static Game base_game, x, y, other;
    static unsigned char bx[65536], by[65536];
    int n_roster = 0, failed = 0, brains = 0;
    const BotRosterEntry *roster = bot_roster(&n_roster);
    for (int e = 0; e < n_roster; e++) {
        if (!bot_roster_linked(e)) continue;
        brains++;
        int8_t keys[3] = { (int8_t)roster[e].strat, (int8_t)roster[e].strat, (int8_t)roster[e].strat };
        unsigned char seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(e * 37 + i * 11 + 5);
        memset(&base_game, 0, sizeof base_game);
        game_set_deal_seed_bytes(seed, 32);
        game_seat_and_deal(&base_game, keys, 3);
        const uint32_t rng_base = bot_drive_seed_base(seed, 32);
        int cycles = 0, same = 1;
        for (int step = 0; step < 300 && base_game.status == GAME_STATUS_PLAYING; step++) {
            BotDriveOut dx, dy, dother;
            memcpy(&x, &base_game, sizeof x);
            memcpy(&y, &base_game, sizeof y);
            game_rng_set(0x13579BDFu);
            random_strategy_set_seed(0x2468ACE0u);
            bot_cycle(&x, rng_base, &dx);
            game_rng_set(0xC0FFEE11u + (uint32_t)step);
            random_strategy_set_seed(0xBADC0DEu ^ (uint32_t)step);
            memcpy(&other, &base_game, sizeof other);
            bot_cycle(&other, rng_base ^ 0x5A5A5A5Au, &dother);
            bot_cycle(&y, rng_base, &dy);
            const int lx = state_put(&x, VIEW_UNMASKED, bx), ly = state_put(&y, VIEW_UNMASKED, by);
            if (dx.n != dy.n || lx != ly || memcmp(bx, by, (size_t)lx) != 0) same = 0;
            if (dx.n <= 0) break;
            memcpy(&base_game, &x, sizeof base_game);
            cycles++;
        }
        fprintf(stderr, "bot seeding self-test: %-18s %3d cycles, %s\n", roster[e].key, cycles,
                same ? "independent of the thread's history" : "DEPENDS ON THE THREAD'S HISTORY");
        if (!same || cycles < 3) failed++;
    }
    fprintf(stderr, "bot seeding self-test: %s (%d brains)\n", failed ? "FAIL" : "OK", brains);
    return failed ? 1 : 0;
}
