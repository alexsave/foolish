#include <stdarg.h>
#include <stdio.h>

#include "world.h"
#include "trace.h"

#define TRACE_BUFS 8
#define TRACE_BUF_LEN 96

static char g_bufs[TRACE_BUFS][TRACE_BUF_LEN];
static int  g_next;

static char *next_buf(void) {
    char *b = g_bufs[g_next];
    g_next = (g_next + 1) % TRACE_BUFS;
    return b;
}

const char *trace_suit(int suit) {
    switch (suit) {
    case SUIT_SPADES:   return "s";
    case SUIT_HEARTS:   return "h";
    case SUIT_CLUBS:    return "c";
    case SUIT_DIAMONDS: return "d";
    default:            return "?";
    }
}

const char *trace_card(Card c) {
    static const char *ranks[] = {"?", "A", "2", "3", "4", "5", "6",
                                  "7", "8", "9", "10", "J", "Q", "K"};
    char *b = next_buf();
    int v = c.value;
    // The deck runs 1..13 with ACE_VALUE 13, so 13 is the ace and 1 is the two.
    const char *r = (v == ACE_VALUE) ? "A" : (v >= 1 && v <= 13) ? ranks[v] : "?";
    snprintf(b, TRACE_BUF_LEN, "%s%s", r, trace_suit(c.suit));
    return b;
}

const char *trace_cards(const Card *c, int n) {
    char *b = next_buf();
    int off = 0;
    for (int i = 0; i < n && off < TRACE_BUF_LEN - 8; i++)
        off += snprintf(b + off, (size_t)(TRACE_BUF_LEN - off), "%s%s",
                        i ? " " : "", trace_card(c[i]));
    if (!n) snprintf(b, TRACE_BUF_LEN, "-");
    return b;
}

const char *trace_move_kind(int kind) {
    static const char *k[] = {"attack", "cover", "pass", "pickup", "good"};
    return (kind >= 0 && kind <= 4) ? k[kind] : "?";
}

const char *trace_event_name(u32 type) {
    static const char *n[] = {"NET->SRV", "NET->CLI", "SRV.SERVICE", "SRV.BOT",
                              "SRV.LOBBY", "CLI.WAKE", "TICK", "HOP"};
    return type < 8 ? n[type] : "?";
}

void trace_emit(World *w, const char *fmt, ...) {
    printf("%10.3f  ", (double)sch_now_us(&w->sch) / 1e6);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
