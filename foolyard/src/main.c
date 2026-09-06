#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "world.h"

#include "bot_roster.h"

// One seat of the table, from a --lineup token: `name@think_ms[+jitter_ms]`.
// The name is a bot roster key (a server-side brain) or a client tier (a
// foolyard client on the far end of a wire), and think_ms means the same thing
// either way - how long that seat takes to answer.
typedef struct SeatSpec {
    u8  is_client;
    i8  strategy;   // bots: roster index
    u8  tier;       // clients: index into CLIENT_TIERS
    u32 think_us;
    u32 jitter_us;
} SeatSpec;

static int seat_spec_parse(const char *tok, SeatSpec *out) {
    char name[32];
    u32 i = 0;
    while (tok[i] && tok[i] != '@' && i < sizeof name - 1) { name[i] = tok[i]; i++; }
    name[i] = 0;

    u32 think_ms = 0, jitter_ms = 0;
    if (tok[i] == '@') {
        const char *p = tok + i + 1;
        think_ms = (u32)strtoul(p, (char **)&p, 10);
        if (*p == '+') jitter_ms = (u32)strtoul(p + 1, NULL, 10);
    }

    memset(out, 0, sizeof *out);
    out->think_us = (u32)(think_ms * MS);
    out->jitter_us = (u32)(jitter_ms * MS);

    int roster = bot_roster_find(name);
    if (roster >= 0) {
        out->is_client = 0;
        out->strategy = (i8)roster;
        return 1;
    }
    for (int t = 0; t < client_tier_count(); t++) {
        if (strcmp(client_impl(t)->name, name) == 0) {
            out->is_client = 1;
            out->tier = (u8)t;
            return 1;
        }
    }

    fprintf(stderr, "unknown seat '%s'. bots: ", name);
    for (int r = 0; r < bot_roster_count(); r++) fprintf(stderr, "%s ", bot_roster_at(r)->key);
    fprintf(stderr, "\n            clients: ");
    for (int t = 0; t < client_tier_count(); t++) fprintf(stderr, "%s ", client_impl(t)->name);
    fprintf(stderr, "\n");
    return 0;
}

static void world_init(World *w, u64 seed, u32 n_games, u32 n_clients) {
    memset(w, 0, sizeof *w);

    sch_init(&w->sch);
    net_init(&w->net);
    w->rng = rng_seed(seed);

    w->n_games = n_games;
    w->games = calloc(n_games, sizeof(GameSlot));
    w->n_clients = n_clients;
    w->clients = calloc(n_clients, sizeof(ClientState));
    w->scratch = calloc(1, sizeof(Game));
    if (!w->games || !w->clients || !w->scratch) {
        fprintf(stderr, "world: out of memory (%u games x %zuKB)\n",
                n_games, sizeof(GameSlot) / 1024);
        exit(1);
    }

    w->scratch_client = -1;

    for (u32 g = 0; g < n_games; g++)
        for (int i = 0; i < MAX_PLAYERS; i++) w->games[g].seat_client[i] = -1;
}

static void usage(void) {
    printf(
"foolyard - a discrete-event Durak table: one simulated server, real kernel\n"
"rules, and clients on modelled wires.\n"
"\n"
"  --lineup SPEC   comma-separated seats, `name@think_ms[+jitter_ms]`.\n"
"                  a name is a bot roster key (server-side brain) or a\n"
"                  client tier (something on the far end of a wire).\n"
"  --games N       tables, all with the same lineup (default 8)\n"
"  --secs N        sim seconds to play (default 300)\n"
"  --seed N        every stream derives from this (default 1)\n"
"  --latency MS    one-way base latency on every client link (default 60)\n"
"  --jitter MS     added per packet, per hop (default 40)\n"
"  --loss PCT      packet loss (default 0)\n"
"  --dup PCT       packet duplication (default 0)\n"
"  --service-us N  how long one request holds a game's lock (default 200)\n"
"  --hiccup-pct N  chance a request stalls behind a long lock hold\n"
"  --hiccup-ms N   how long that stall lasts\n"
"  --stall-secs N  quiet time on a live game that counts as a stall (30)\n"
"  --kernel-pacing 0|1  bots also wait the kernel's human-watching pace (1)\n"
"  --deep          every-change card walk + clone/compare on rejects (slow)\n"
"  --no-checks     drop the detectors entirely, for capacity runs\n"
"  --csv           per-seat CSV lines as well, for the sweep driver\n"
"  --trace         one line per scheduler pop, plus what each event did\n"
"  --until-games N stop once N games have finished, whatever the clock says\n"
"\n"
"examples\n"
"  foolyard --lineup octogen@50,octogen@3000\n"
"      the same brain at two speeds, to see what latency alone decides\n"
"  foolyard --lineup wellbehaved@200,laggy@900,cordite@400,random@40\n"
"      humans on wires against bots that think at different rates\n"
"  foolyard --lineup wellbehaved@150,reconnect@150,octogen@100 --loss 3 --jitter 300\n"
"      a hostile wire under a fast bot\n");
}

// tiltyard's main loop: pop the next event forever, until the kill event.
static u64 run(World *w, u64 duration_us) {
    sch_schedule(&w->sch, event_of(EV_TICK, TICK_KILL), duration_us);
    sch_schedule(&w->sch, event_of(EV_TICK, TICK_SWEEP), w->knobs.sweep_period_us);

    u64 events = 0;
    for (;;) {
        u32 ev = sch_pop(&w->sch);
        if (ev == SCH_EMPTY) break;
        events++;

        u32 param = event_param(ev);
        u32 type = event_type(ev);

        // Every pop, before it is handled: the handlers add an indented line
        // for what it actually did.
        if (w->knobs.trace) {
            const char *name = trace_event_name(type);
            if (type == EV_SRV_BOT)
                trace_line(w, "%-11s g%u seat %u", name, bot_param_game(param), bot_param_seat(param));
            else if (type == EV_SRV_SERVICE || type == EV_SRV_LOBBY)
                trace_line(w, "%-11s g%u", name, param);
            else if (type == EV_NET_TO_SERVER || type == EV_NET_TO_CLIENT) {
                Packet *p = net_pkt(w, param);
                trace_line(w, "%-11s g%u seat %u  pkt#%u kind %u len %u",
                           name, p->game_id, p->seat, param, p->kind, p->len);
            } else if (type == EV_CLI_WAKE)
                trace_line(w, "%-11s client %u", name, param);
            else
                trace_line(w, "%-11s %u", name, param);
        }

        switch (type) {
        case EV_NET_TO_SERVER: srv_on_packet(w, param); break;
        case EV_NET_TO_CLIENT: client_on_packet(w, param); break;
        case EV_SRV_SERVICE:   srv_on_service(w, param); break;
        case EV_SRV_BOT:       srv_on_bot(w, param); break;
        case EV_SRV_LOBBY:     srv_on_lobby(w, param); break;
        case EV_CLI_WAKE:      client_on_wake(w, param); break;
        case EV_TICK:
            if (param == TICK_KILL) return events;
            inv_sweep(w);
            sch_schedule(&w->sch, event_of(EV_TICK, TICK_SWEEP), w->knobs.sweep_period_us);
            break;
        default: break;
        }

        if (w->knobs.stop_after && w->finished >= w->knobs.stop_after) return events;
    }
    return events;
}

// Seat every game with the same lineup, then deal. A client seat gets a wire
// (its tier fills in the link), a bot seat gets a brain and its own think time.
static void seat_tables(World *w, const SeatSpec *lineup, int n_seats) {
    u32 next_client = 0;

    for (u32 g = 0; g < w->n_games; g++) {
        GameSlot *s = &w->games[g];

        for (int seat = 0; seat < n_seats; seat++) {
            const SeatSpec *spec = &lineup[seat];
            if (!spec->is_client) {
                // strategy_key is a STRAT_* brain id, not a roster index:
                // bot_drive resolves it with bot_roster_find_by_strat.
                s->bots[seat].strategy = (i8)bot_roster_at(spec->strategy)->strat;
                s->bots[seat].think_us = spec->think_us;
                s->bots[seat].jitter_us = spec->jitter_us;
                continue;
            }

            ClientState *cs = &w->clients[next_client];
            cs->used = 1;
            cs->id = (u16)next_client;
            cs->tier = spec->tier;
            cs->seat = (u8)seat;
            cs->game_id = (u16)g;
            cs->rng = rng_seed(w->rng ^ ((u64)next_client << 32));
            cs->think_us = spec->think_us;
            cs->think_jitter_us = spec->jitter_us;

            cs->link.up_us = w->knobs.base_latency_us;
            cs->link.down_us = w->knobs.base_latency_us;
            cs->link.jitter_us = w->knobs.jitter_us;
            cs->link.loss_pct = w->knobs.loss_pct;
            cs->link.dup_pct = w->knobs.dup_pct;
            cs->link.ordered = 1;              // a tier may switch to datagrams
            client_impl(cs->tier)->settings(cs);

            s->seat_client[seat] = (i16)next_client;
            next_client++;
        }

        srv_open_game(w, (u16)g, n_seats);

        for (int seat = 0; seat < n_seats; seat++)
            if (s->seat_client[seat] >= 0)
                client_subscribe(w, &w->clients[s->seat_client[seat]]);
    }
}

static int lineup_parse(const char *str, SeatSpec *out, int cap) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", str);

    int n = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        if (n == cap) { fprintf(stderr, "at most %d seats\n", cap); return -1; }
        if (!seat_spec_parse(tok, &out[n])) return -1;
        n++;
    }
    return n;
}

static void report(World *w, const SeatSpec *lineup, int n_seats, u64 events, u64 sim_us, double wall_s) {
    printf("\n%llu events over %.1f sim-seconds in %.2fs wall (%.0f events/s)\n",
           (unsigned long long)events, (double)sim_us / 1e6, wall_s,
           wall_s > 0 ? (double)events / wall_s : 0.0);

    printf("  games      %llu dealt, %llu finished\n",
           (unsigned long long)w->deals, (unsigned long long)w->finished);
    printf("  moves      %llu sent, %llu applied, %llu rejected, %llu applied against a board the mover had not seen\n",
           (unsigned long long)w->moves_sent, (unsigned long long)w->moves_applied,
           (unsigned long long)w->moves_rejected, (unsigned long long)w->stale_accepts);
    printf("  bots       %llu actions\n", (unsigned long long)w->bot_actions);
    printf("  packets    %llu sent, %llu delivered, %llu dropped, %llu duplicated, %llu overtaken; peak %u in flight of %d\n",
           (unsigned long long)w->net.sent, (unsigned long long)w->net.delivered,
           (unsigned long long)w->net.dropped, (unsigned long long)w->net.duplicated,
           (unsigned long long)w->net.reordered, w->net.packets.high_water, ID_LIMIT);

    if (w->knobs.csv) {
        for (int i = 0; i < n_seats; i++) {
            const SeatSpec *sp = &lineup[i];
            printf("CSV,%d,%d,%s,%u,%llu,%llu,%llu\n", n_seats, i,
                   sp->is_client ? client_impl(sp->tier)->name : bot_roster_at(sp->strategy)->key,
                   sp->think_us / (u32)MS, (unsigned long long)w->fools[i],
                   (unsigned long long)w->finished, (unsigned long long)w->seat_moves[i]);
        }
    }

    // Who ends up the fool, by seat. With the same brain at two speeds this is
    // the whole experiment: any imbalance here was bought with latency alone.
    printf("  seats      ");
    for (int i = 0; i < n_seats; i++) {
        const SeatSpec *sp = &lineup[i];
        printf("%s%s@%ums fool %llu/%llu, %llu moves", i ? " | " : "",
               sp->is_client ? client_impl(sp->tier)->name : bot_roster_at(sp->strategy)->key,
               sp->think_us / (u32)MS,
               (unsigned long long)w->fools[i], (unsigned long long)w->finished,
               (unsigned long long)w->seat_moves[i]);
    }
    printf("\n");

    inv_print(w);
}

int main(int argc, char **argv) {
    Knobs k = {
        .base_latency_us = 60 * (u32)MS,
        .jitter_us       = 40 * (u32)MS,
        .loss_pct        = 0,
        .dup_pct         = 0,
        .srv_service_us  = 200,
        .bot_kernel_pacing = 1,
        .stall_us        = 30 * SEC,
        .sweep_period_us = 5 * SEC,
        .lobby_delay_us  = 2 * SEC,
        .checks          = 1,
        .deep            = 0,
        .verbose         = 0,
    };
    u64 seed = 1, secs = 300;
    u32 games = 8;
    const char *lineup_str = "wellbehaved@200,laggy@800,handwritten@300,octogen@600";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--deep")) { k.deep = 1; k.checks = 2; }
        else if (!strcmp(a, "--no-checks")) k.checks = 0;
        else if (!strcmp(a, "--csv")) k.csv = 1;
        else if (!strcmp(a, "--trace")) k.trace = 1;
        else if (!strcmp(a, "--verbose")) k.verbose = 1;
        else if (!v) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        else if (!strcmp(a, "--lineup")) { lineup_str = v; i++; }
        else if (!strcmp(a, "--games")) { games = (u32)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--until-games")) { k.stop_after = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--secs")) { secs = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--seed")) { seed = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--latency")) { k.base_latency_us = (u32)(strtoul(v, NULL, 10) * MS); i++; }
        else if (!strcmp(a, "--jitter")) { k.jitter_us = (u32)(strtoul(v, NULL, 10) * MS); i++; }
        else if (!strcmp(a, "--loss")) { k.loss_pct = (u32)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--dup")) { k.dup_pct = (u32)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--service-us")) { k.srv_service_us = (u32)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--hiccup-pct")) { k.hiccup_pct = (u32)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--hiccup-ms")) { k.hiccup_us = (u32)(strtoul(v, NULL, 10) * MS); i++; }
        else if (!strcmp(a, "--stall-secs")) { k.stall_us = strtoull(v, NULL, 10) * SEC; i++; }
        else if (!strcmp(a, "--kernel-pacing")) { k.bot_kernel_pacing = (u8)strtoul(v, NULL, 10); i++; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }

    SeatSpec lineup[MAX_PLAYERS];
    int n_seats = lineup_parse(lineup_str, lineup, MAX_PLAYERS);
    if (n_seats < 0) return 2;
    if (n_seats < 2) { fprintf(stderr, "a table needs at least 2 seats\n"); return 2; }

    u32 per_game = 0;
    for (int i = 0; i < n_seats; i++) if (lineup[i].is_client) per_game++;
    if (games == 0 || games > MAX_GAMES) { fprintf(stderr, "1..%d games\n", MAX_GAMES); return 2; }
    u32 n_clients = games * per_game;
    if (n_clients > MAX_CLIENTS) { fprintf(stderr, "%u clients, cap is %d\n", n_clients, MAX_CLIENTS); return 2; }

    printf("foolyard: %u tables x %d seats, seed %llu, %llu sim-seconds\n",
           games, n_seats, (unsigned long long)seed, (unsigned long long)secs);
    printf("  lineup   ");
    for (int i = 0; i < n_seats; i++)
        printf("%s%s@%ums%s", i ? ", " : "",
               lineup[i].is_client ? client_impl(lineup[i].tier)->name
                                   : bot_roster_at(lineup[i].strategy)->key,
               lineup[i].think_us / (u32)MS, lineup[i].is_client ? "" : " (bot)");
    if (per_game)
        printf("\n  wire     %ums +%ums jitter, %u%% loss, %u%% dup\n",
               k.base_latency_us / (u32)MS, k.jitter_us / (u32)MS, k.loss_pct, k.dup_pct);
    else
        printf("\n  wire     unused: every seat is a server-side bot, so nothing is sent.\n"
               "           a bot seat's own timing is name@think+jitter in the lineup.\n");

    World w;
    world_init(&w, seed, games, n_clients ? n_clients : 1);
    w.knobs = k;
    seat_tables(&w, lineup, n_seats);

    clock_t t0 = clock();
    u64 events = run(&w, secs * SEC);
    double wall = (double)(clock() - t0) / CLOCKS_PER_SEC;

    report(&w, lineup, n_seats, events, sch_now_us(&w.sch), wall);

    int failed = 0;
    for (int i = 0; i < FIND_COUNT; i++) if (w.findings.count[i]) failed = 1;

    sch_free(&w.sch);
    net_free(&w.net);
    free(w.games);
    free(w.clients);
    free(w.scratch);
    return failed;
}
