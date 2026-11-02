// orch.h: a sampled orchestra shared by the classical cities. Real recordings: VSCO 2 CE (string sections, brass,
// winds, harp, glockenspiel), VCSL (tenor sax, timpani, field drum, cymbals, tubular bells, pipe organ, vibraphone),
// Salamander piano; the GM choir (Apple DLS) layered with the formant choir where a score asks for voices.
// Mockup rules from the research: sustained sections start early (strings 30 ms, brass 20-40 ms, choir 90 ms), every
// solo brass/wind part is doubled by a second player with its own detune and timing so it sounds like a section,
// seating pans, depth by reverb send (strings/winds wetter, brass mid, percussion driest), legato overlap on sustains.
#pragma once
#include "render.h"
#include "scoretxt.h"
#define OM "../samples/manifests/"

typedef struct { const char *part; int t, t2; double tp, dp; int drum; } OPart;
static OPart g_op[64]; static int g_nop;
static int g_o_sustain[64], g_o_nsus;   // sustained tracks: legato overlap in o_post
static int o_smp(const char *name, const char *man, double rel, int loop, double pan, double delay_ms, double send, double gain) {
    int t = smp_track(name, man); g_tr[t].sp[0] = rel; g_tr[t].sp[10] = loop; g_tr[t].sp[2] = loop ? 25 : 0;
    g_tr[t].pan = pan; g_tr[t].delay_ms = delay_ms; g_tr[t].send[0] = send; g_tr[t].gain_db = gain; g_tr[t].human_ms = loop ? 6 : 4; g_tr[t].human_vel = 0.05;
    // sustained samples: skip the recorded bow/breath swell by note length (it read as a reversed note), so the early start shrinks
    if (loop) { g_tr[t].sp[12] = 1; g_tr[t].delay_ms = delay_ms * 0.35; }
    // strings: an audible vibrato on top of the recorded one (12 cents at 5.6 Hz, each note its own rate and phase, onset after
    // 0.18 s): a blind review measured no vibrato at all in the round 2 strings
    if (loop && (strstr(man, "vln_") || strstr(man, "vla_") || strstr(man, "vc_") || strstr(man, "cb_") || strstr(man, "svln"))) { g_tr[t].sp[13] = 12; g_tr[t].sp[14] = 5.6; }
    eq(t, 2, 40, 0.7, 0); return t;
}
// a section from a solo instrument: a second player, slightly detuned and late, a little apart in the stereo field
static int o_double(int t, const char *name, double pan_off) {
    Track *a = &g_tr[t]; int u = smp_track(name, ((SampleSet *)a->user)->name); Track *b = &g_tr[u];
    memcpy(b->sp, a->sp, sizeof a->sp); b->sp[6] = 9; b->pan = clampd(a->pan + pan_off, -1, 1); b->delay_ms = a->delay_ms + 14; b->send[0] = a->send[0] * 1.2;
    b->gain_db = a->gain_db - 2; b->human_ms = 9; b->human_vel = 0.08; memcpy(b->eq, a->eq, sizeof a->eq); b->neq = a->neq;
    if (b->sp[10] > 0) { b->sp[13] = fmax(b->sp[13], 7); b->sp[14] = 5.2; }   // a sustained second player: its own vibrato, at its own rate
    return u;
}
// high violins: the section samples stop at D6 and stretching them above it sounded synthetic, so notes from minp up go to three
// solo violins (VSCO solo arco with vibrato, recorded to C7), each its own player in timing, detune and pan
typedef struct { int src, dst[3], minp; } OSplit;
static OSplit g_osp[8]; static int g_nosp;
static void o_hi(int src, const char *name, int minp) {
    Track *a = &g_tr[src]; char nm[48];
    int d0 = o_smp(name, OM "svln_vib.txt", a->sp[0], 1, a->pan, -25, a->send[0] * 1.1, a->gain_db - 2);
    snprintf(nm, sizeof nm, "%s 2", name); int d1 = o_double(d0, nm, 0.12);
    snprintf(nm, sizeof nm, "%s 3", name); int d2 = o_double(d0, nm, -0.12); g_tr[d2].delay_ms += 8; g_tr[d2].sp[6] = 6; g_tr[d2].gain_db = g_tr[d0].gain_db - 1;
    memcpy(g_tr[d0].eq, a->eq, sizeof a->eq); g_tr[d0].neq = a->neq;
    g_osp[g_nosp++] = (OSplit){src, {d0, d1, d2}, minp}; g_o_sustain[g_o_nsus++] = d0; g_o_sustain[g_o_nsus++] = d1; g_o_sustain[g_o_nsus++] = d2;
}
static void o_part(const char *part, int t, int t2, double tp, int drum, double dp) { g_op[g_nop++] = (OPart){part, t, t2, tp, dp, drum}; }
static int o_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    for (int i = 0; i < g_nop; i++) if (!strcmp(p, g_op[i].part)) { *tp = g_op[i].tp; if (g_op[i].dp) *dp = g_op[i].dp; return g_op[i].t; }
    // a suffix we do not know: fall back to the bare instrument
    char base[48]; snprintf(base, sizeof base, "%s", p); char *sp = strchr(base, ' '); if (sp) { *sp = 0; for (int i = 0; i < g_nop; i++) if (!strcmp(base, g_op[i].part)) { *tp = g_op[i].tp; return g_op[i].t; } }
    return -1;
}
// after the score: second players, drums pinned to their sample pitch, legato overlap on sustained parts
static void o_post(int n0) {
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) for (int k = 0; k < g_nosp; k++) if (g_note[i].tr == g_osp[k].src && g_note[i].pitch >= g_osp[k].minp) {
        Note n = g_note[i]; g_note[i].tr = g_osp[k].dst[0];
        for (int d = 1; d < 3; d++) { Note *c = note(g_osp[k].dst[d], n.beat, n.len, n.pitch, n.vel * (d == 1 ? 0.92 : 0.88)); c->p[0] = n.p[0]; }
    }
    for (int i = n0; i < n1; i++) {
        Note n = g_note[i];
        for (int k = 0; k < g_nop; k++) if (g_op[k].t == n.tr) {
            if (g_op[k].drum) g_note[i].pitch = 60;
            if (g_op[k].t2 >= 0) { Note *c = note(g_op[k].t2, n.beat, n.len, n.pitch, n.vel * 0.92); c->p[0] = n.p[0]; }
            break;
        }
        for (int s = 0; s < g_o_nsus; s++) if (g_o_sustain[s] == n.tr && n.len >= 0.9) g_note[i].len += 0.06;
    }
}
static void o_hall(double rt, double pre_ms) {
    g_bus[0] = (Bus){.type = 1, .rev = {rt, rt * 0.5, pre_ms, 1.4, 180, 7000, 0.14, 1.0, 0.2, 0.25}, .gain_db = -5, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 28, 0.7, 0}; g_master.eq[1] = (EqBand){3, 280, 0.8, -1.5}; g_master.eq[2] = (EqBand){5, 6000, 0, 3}; g_master.neq = 3;
    g_master.cthr = -22; g_master.cratio = 1.8; g_master.catt = 0.03; g_master.crel = 0.25;
    // round 4 texture: mono below 120 Hz, a light tape stage, a hall's room tone far down, the very top rolled off gently
    g_master.width_lo_hz = 120; g_master.tape = 0.3; g_master.room_db = -72; g_master.top_hz = 18500;
    g_master.eq[3] = (EqBand){4, 70, 0, 3}; g_master.neq = 4;   // sub weight: the orchestral cities measured -26 to -49 dB under 60 Hz
    g_tail_s = 8;
}
// roll the chords of one track upward: each note above the lowest starts ms later (a hand's spread, a strum); bpm converts
static void o_roll(int n0, int tr, double ms, double bpm) {
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) if (g_note[i].tr == tr) { int below = 0;
        for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == tr && fabs(g_note[j].beat - g_note[i].beat) < 1e-6 && g_note[j].pitch < g_note[i].pitch) below++;
        if (below) g_note[i].beat += below * ms / 1000 * bpm / 60; }
}
