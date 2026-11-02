// Miami - 2 a.m. (round 4). A minor-key melodic / deep house suite at 122 BPM in F minor with Miami's Cuban roots underneath.
// Score text by Fable (score.txt; the plan is in fable_suite_v4.md). Real recordings where they exist: Salamander piano,
// VSCO strings (with vibrato) and marimba, VCSL congas, bongos and shaker, Big Rusty Drums kick/snare/hats layered with a
// synthesized sub kick; synthesis for the Rhodes (with an FM e-piano layer), the dark pluck, the pad, the sub and the bass.
// Production: everything melodic pumps against the kick, the low end keeps real stereo width (Kygo/Flume measure -10 to -14 dB
// side/mid under 150 Hz), a tape stage and a gentle top roll-off on the master, humanised percussion with accents.
#include "../engine/orch.h"
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)

static int KICK, KSUB, CLAP, SNARE, HAT, OHAT, SHAKER, CONGA, BONGO, CRASH, RISER, PIANO, RHODES, RHODESFM, PLUCK, MARIMBA,
           VLN, VLA, VC, PAD, SUB, BASS, BASSW;
typedef struct { const char *name; int *t; double dp; } PMap;
static PMap g_pm[] = {{"KICK", &KICK, 60}, {"CLAP", &CLAP, 60}, {"SNARE", &SNARE, 60}, {"HAT", &HAT, 60}, {"OHAT", &OHAT, 60},
    {"SHAKER", &SHAKER, 60}, {"CONGA", &CONGA, 0}, {"BONGO", &BONGO, 0}, {"CRASH", &CRASH, -60}, {"RISER", &RISER, -60},
    {"PIANO", &PIANO, 0}, {"RHODES", &RHODES, 0}, {"PLUCK", &PLUCK, 0}, {"MARIMBA", &MARIMBA, 0}, {"STRINGS", &VLN, 0},
    {"PAD", &PAD, 0}, {"SUB", &SUB, 0}, {"BASS", &BASS, 0}};
static int map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    for (unsigned i = 0; i < sizeof g_pm / sizeof *g_pm; i++) if (!strcmp(p, g_pm[i].name)) { if (g_pm[i].dp) *dp = g_pm[i].dp; return *g_pm[i].t; }
    return -1;
}

void city_setup(void) {
    g_on_done = sections_out;
    // ---- drums: sampled kick on top of a synthesized sub kick; sampled snare and hats; the clap synthesized and wide ----
    KICK = o_smp("kick", OM "brd_kick_cl.txt", 0.2, 0, 0, 0, 0, 0); eq(KICK, 2, 45, 0.7, 0); eq(KICK, 3, 3500, 1, 3);
    KSUB = syn_track("kick sub", syn_kick); SP(KSUB, 46, 180, 0.03, 0.32, 0.4, 1.2, 0.2, 0.06); g_tr[KSUB].gain_db = -9; eq(KSUB, 1, 220, 0.7, 0);
    CLAP = syn_track("clap", syn_clap); SP(CLAP, 1100, 1.0, 0.1, 9, 4, 600); g_tr[CLAP].gain_db = -6; g_tr[CLAP].send[0] = 0.25; g_tr[CLAP].width = 1.2;
    SNARE = o_smp("snare", OM "brd_snare_top.txt", 0.2, 0, 0, 0, 0.2, 0);
    HAT = o_smp("hat", OM "brd_hat_cl.txt", 0.08, 0, 0.25, 0, 0.08, 5); OHAT = o_smp("ohat", OM "brd_hat_open.txt", 0.3, 0, -0.25, 0, 0.1, -3);
    SHAKER = o_smp("shaker", OM "vcsl_shaker.txt", 0.08, 0, -0.35, 0, 0.1, 14);
    CONGA = o_smp("conga", OM "latin_conga.txt", 0.3, 0, 0.3, 0, 0.15, 6); BONGO = o_smp("bongo", OM "latin_bongo.txt", 0.3, 0, -0.3, 0, 0.15, 3);
    CRASH = o_smp("crash", OM "crash_oh.txt", 2.0, 0, 0.2, 0, 0.3, -8);
    RISER = syn_track("riser", syn_riser); SP(RISER, 300, 9000, 2.0); g_tr[RISER].gain_db = -18; g_tr[RISER].send[0] = 0.4;
    for (int t = KICK; t <= CRASH; t++) { g_tr[t].human_ms = fmax(g_tr[t].human_ms, 4); g_tr[t].human_vel = 0.08; }
    g_tr[CONGA].human_ms = g_tr[BONGO].human_ms = 7; g_tr[CONGA].human_vel = g_tr[BONGO].human_vel = 0.12;
    g_tr[KICK].human_ms = g_tr[KSUB].human_ms = 1;   // the four-on-the-floor stays on the grid; the hands around it breathe
    // ---- keys ----
    PIANO = o_smp("piano", OM "salamander.txt", 0.6, 0, -0.15, 0, 0.3, 3); g_tr[PIANO].sp[1] = 0.5; eq(PIANO, 3, 300, 1, -2);
    RHODES = syn_track("rhodes", syn_epiano); SP(RHODES, 1.6, 0.45, 0.2, 2.4, 0.3, 0.25, 4.0, 0.6); g_tr[RHODES].gain_db = -6; g_tr[RHODES].pan = 0.25; g_tr[RHODES].send[0] = 0.35;
    RHODESFM = o_smp("rhodes fm", OM "fmpiano.txt", 0.4, 0, 0.3, 0, 0.35, -8); g_tr[RHODESFM].chorus_mix = 0.4; g_tr[RHODESFM].chorus_ms = 7; g_tr[RHODESFM].chorus_depth_ms = 1.5;
    PLUCK = syn_track("pluck", syn_saw); SP(PLUCK, 3, 10, 1.0, 9, 0.09, 0.7, 0.22, 0.12, 0.25, 0.001); g_tr[PLUCK].gain_db = -1; g_tr[PLUCK].send[0] = 0.25; g_tr[PLUCK].send[1] = 0.25;
    g_tr[PLUCK].chorus_mix = 0.3; g_tr[PLUCK].chorus_ms = 8; g_tr[PLUCK].chorus_depth_ms = 1.5; eq(PLUCK, 2, 180, 0.7, 0);
    MARIMBA = o_smp("marimba", OM "marimba.txt", 1.0, 0, -0.45, 0, 0.2, 2);
    // ---- strings (split into sections by register), pad, sub and bass ----
    VLN = o_smp("violins", OM "vln_sus.txt", 0.5, 1, -0.4, -30, 0.45, 0); VLA = o_smp("violas", OM "vla_sus.txt", 0.5, 1, 0.1, -30, 0.45, -2);
    VC = o_smp("cellos", OM "vc_sus.txt", 0.5, 1, 0.35, -30, 0.4, -2); o_hi(VLN, "violins hi", 88);
    PAD = syn_track("pad", syn_pad); SP(PAD, 7, 14, 2.5, 0.6, 0.8, 0.6, 600, 0); g_tr[PAD].gain_db = -12; g_tr[PAD].send[0] = 0.4; eq(PAD, 2, 160, 0.7, 0);
    g_tr[PAD].chorus_mix = 0.4; g_tr[PAD].chorus_ms = 10; g_tr[PAD].chorus_depth_ms = 2.5; g_tr[PAD].chorus_hz = 0.25;
    SUB = syn_track("sub", syn_sub); SP(SUB, 0.006, 0.08, 0.15, 0.4, 0); g_tr[SUB].gain_db = -14; eq(SUB, 1, 120, 0.7, 0);
    BASS = syn_track("bass", syn_saw); SP(BASS, 2, 8, 1.2, 4, 0.08, 0.9, 0.3, 0.06, 0.3, 0.002); g_tr[BASS].gain_db = -7; eq(BASS, 2, 70, 0.7, 0); eq(BASS, 1, 2400, 0.7, 0);
    g_tr[BASS].drive = 0.6; g_tr[BASS].drive_mix = 0.5;
    // a wide layer of the bass a fifth of the way out on each side: the low end gets the references' stereo width
    BASSW = syn_track("bass wide", syn_saw); SP(BASSW, 4, 22, 1.4, 4, 0.08, 0.9, 0.3, 0.06, 0, 0.002); g_tr[BASSW].gain_db = -8; g_tr[BASSW].width = 1.6;
    g_tr[BASSW].haas_ms = 9; eq(BASSW, 2, 90, 0.7, 0); eq(BASSW, 1, 1500, 0.7, 0);
    // everything melodic pumps against the kick
    int duck[] = {PAD, RHODES, RHODESFM, PLUCK, BASS, BASSW, SUB, VLN, VLA, VC, PIANO};
    for (unsigned i = 0; i < sizeof duck / sizeof *duck; i++) { g_tr[duck[i]].duck_src = KICK; g_tr[duck[i]].duck_db = duck[i] == SUB || duck[i] == BASS ? 6 : 3; g_tr[duck[i]].duck_rel = 0.12; }
    // buses: a dark hall, a plate, and the pluck's dotted-eighth ping-pong
    g_bus[0] = (Bus){.type = 1, .rev = {2.6, 1.0, 22, 1.3, 250, 6500, 0.12, 1.0, 0.25, 0.25}, .gain_db = -6, .duck_src = -1};
    g_bus[1] = (Bus){.type = 2, .dl_s = 60.0 / 122 * 0.75, .dr_s = 60.0 / 122 * 0.75, .fb = 0.3, .lp = 4500, .hp = 400, .gain_db = -12, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 25, 0.7, 0}; g_master.eq[1] = (EqBand){3, 350, 0.8, -1.5}; g_master.eq[2] = (EqBand){5, 3000, 0, 5}; g_master.eq[3] = (EqBand){3, 50, 0.8, -4}; g_master.neq = 4;   // against Kygo and Flume: 10 dB dark above 1 kHz, 10-16 dB heavy under 60 Hz
    g_master.cthr = -20; g_master.cratio = 1.8; g_master.catt = 0.01; g_master.crel = 0.12; g_master.width_lo_hz = 0;
    g_master.tape = 0.4; g_master.top_hz = 18000;
    g_tail_s = 6; g_dyn_depth = 1.0;
}

// Fable's FX lines "FILTER <PART> <hz0> -> <hz1> bars N-M": low-pass sweeps inside that section; the filter holds where it
// ends, and opens fully afterwards only when the sweep ends wide open (6 kHz and up)
static void fx_filters(void) {
    FILE *f = fopen("score.txt", "r"); if (!f) return; char line[2048], sec[64] = ""; int si = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "SECTION ", 8)) { char *b = strstr(line, " BARS "); if (b) { int L = (int)(b - line - 8); snprintf(sec, sizeof sec, "%.*s", L > 47 ? 47 : L, line + 8); si = sec_find(sec); } continue; }
        char part[32]; double a, z; int n0, n1;
        if (si >= 0 && sscanf(line, "FX: FILTER %31s %lf -> %lf bars %d-%d", part, &a, &z, &n0, &n1) == 5) {
            double tp, vs, dp = 60; int t = map(part, &tp, &vs, &dp); if (t < 0) continue;
            int ts[4] = {t, -1, -1, -1}; if (t == RHODES) ts[1] = RHODESFM; if (t == KICK) ts[1] = KSUB; if (t == VLN) { ts[1] = VLA; ts[2] = VC; } if (t == BASS) ts[1] = BASSW;
            for (int k = 0; k < 4 && ts[k] >= 0; k++) sec_lp(ts[k], sec, n0, n1, a, z, z < 6000);
        }
    }
    fclose(f);
}
static void split_strings(int n0) {   // chords on STRINGS: top to the violins, middle to the violas, bottom to the cellos
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { if (g_note[i].tr != VLN) continue; int idx[8], c = 0;
        for (int j = n0; j < n1 && c < 8; j++) if (g_note[j].tr == VLN && fabs(g_note[j].beat - g_note[i].beat) < 1e-6) idx[c++] = j;
        if (c < 2 || idx[0] != i) continue;
        for (int a = 0; a < c; a++) for (int b = a + 1; b < c; b++) if (g_note[idx[b]].pitch < g_note[idx[a]].pitch) { int x = idx[a]; idx[a] = idx[b]; idx[b] = x; }
        for (int a = 0; a < c - 1; a++) g_note[idx[a]].tr = a == 0 && c >= 3 ? VC : VLA; }
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", map, 0);
    split_strings(n0);
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { Note n = g_note[i];
        if (n.tr == KICK) note(KSUB, n.beat, n.len, 60, n.vel);
        if (n.tr == RHODES) note(RHODESFM, n.beat, n.len, n.pitch, n.vel * 0.8);
        if (n.tr == BASS) note(BASSW, n.beat, n.len, n.pitch, n.vel * 0.9);
        if (n.tr == HAT || n.tr == SHAKER) { double fr = n.beat - floor(n.beat); g_note[i].vel *= fr < 0.01 ? 1.0 : fabs(fr - 0.5) < 0.01 ? 0.8 : 0.62; }
        if (n.tr == SUB && n.pitch < 29) g_note[i].pitch += 12;   // F1 and below sit an octave up, where a sub is felt and heard
    }
    o_post(n0);
    fx_filters();
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
