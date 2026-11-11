// Seoul - K-pop suite over Jersey club, with a 90s NY garage-house part (round 3). Score text by Fable (score.txt).
// Jersey drums per the round-2 research; house sounds carried over from New York round 2 (M1 organ recipe, M1 piano,
// choked off-beat hats, swung 16ths); thick stacked leads (supersaw three octaves + square layer), real VSCO strings
// and brass, Salamander piano.
#include "../engine/render.h"
#include "../engine/scoretxt.h"
#define SM "../samples/manifests/"
static int SSTR, SBRS, KICK, KTOP, CLAP, SNARE, GHOST, HAT, OHAT, SQK, SHK, CRASH, IMPACT, RISER, ROLL,
           LEAD, LEADLO, LEADHI, LEADSQ, LEAD2, ARP, STAB, BELL, PAD, PIANO, M1P, ORGAN, B808, SUB, VLN, VLA, VC, TPT, TBN, HRN, CHANT;
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)
static int smp(const char *name, const char *man, double rel, int loop) { int t = smp_track(name, man); g_tr[t].sp[0] = rel; g_tr[t].sp[10] = loop; return t; }

void city_setup(void) {
    g_on_done = sections_out;
    // ---- drums ----
    KICK = syn_track("kick", syn_kick);   SP(KICK, 58, 340, 0.010, 0.08, 0.0, 2.5, 0.09, 0.05); g_tr[KICK].gain_db = -5; eq(KICK, 2, 32, 0.8, 0); eq(KICK, 3, 60, 1.0, 4); eq(KICK, 3, 120, 1.2, 2); g_tr[KICK].group = 1;
    KTOP = syn_track("kicktop", syn_rim); SP(KTOP, 3800, 1600, 0.004); g_tr[KTOP].gain_db = -14; g_tr[KTOP].group = 1;
    CLAP = syn_track("clap", syn_clap);   SP(CLAP, 1450, 1.2, 0.07, 6, 3, 900); g_tr[CLAP].gain_db = -6; g_tr[CLAP].send[0] = 0.15; g_tr[CLAP].group = 1;
    SNARE = smp("snare", SM "brd_snare_top.txt", 0.15, 0); g_tr[SNARE].gain_db = 2; g_tr[SNARE].group = 1;
    GHOST = syn_track("ghost", syn_snare); SP(GHOST, 210, 0.03, 0.06, 0.8, 2500, 0.2); g_tr[GHOST].gain_db = -10; g_tr[GHOST].pan = 0.2; g_tr[GHOST].group = 1;
    HAT = syn_track("hat", syn_hat);      SP(HAT, 0.025, 2.1, 0.4, 8000, 11000, 0, 0.001); g_tr[HAT].gain_db = -8; g_tr[HAT].pan = 0.25; g_tr[HAT].group = 1; g_tr[HAT].human_vel = 0.1;
    OHAT = syn_track("ohat", syn_hat);    SP(OHAT, 0.2, 1.9, 0.55, 6500, 9500, 1, 0.002); g_tr[OHAT].gain_db = -9; g_tr[OHAT].pan = -0.2; g_tr[OHAT].group = 1;
    SQK = syn_track("squeak", syn_chirp); SP(SQK, 1900, 2800, 0.05, 0.035, 0.3); g_tr[SQK].gain_db = -12; g_tr[SQK].send[0] = 0.15; g_tr[SQK].pan = 0.3;
    g_tr[SQK].mute = 1;   // round 4: Alex's "little 'wiip' sounds" and the reviewer's 2-2.8 kHz zap: this 1.9->2.8 kHz bed squeak. Removed, nothing replaces it
    SHK = smp("shaker", SM "vcsl_shaker.txt", 0.08, 0); g_tr[SHK].gain_db = -2; g_tr[SHK].pan = -0.35; g_tr[SHK].group = 1;
    CRASH = smp("crash", SM "crash_oh.txt", 1.6, 0); g_tr[CRASH].gain_db = -9; g_tr[CRASH].pan = 0.25;
    IMPACT = syn_track("impact", syn_808); SP(IMPACT, 0.5, 1.5, 24, 0.3, 0.6); g_tr[IMPACT].gain_db = -9; g_tr[IMPACT].send[1] = 0.3;
    RISER = syn_track("riser", syn_riser); SP(RISER, 300, 10000, 2.0); g_tr[RISER].gain_db = -16; g_tr[RISER].send[1] = 0.3;
    ROLL = syn_track("roll", syn_snare);  SP(ROLL, 200, 0.04, 0.09, 0.9, 1500, 0.3); g_tr[ROLL].gain_db = -10; g_tr[ROLL].send[0] = 0.25;
    g_grp[1] = (Group){.drive = 0.35, .cthr = -14, .cratio = 2.5, .catt = 0.006, .crel = 0.08, .parallel = 0.6};
    // ---- the lead: supersaw in three octaves + a detuned square layer (the "girl-crush" bite) ----
    LEAD = syn_track("lead", syn_supersaw);   SP(LEAD, 11, 30, 0.4, 2.5, 12, 0.12, 0.003, 0, 1, 0.12, 0, 0.9, 0.9, 0.9, 900);
    LEADLO = syn_track("leadlo", syn_supersaw); SP(LEADLO, 7, 22, 0.5, 2, 6, 0.15, 0.003, 0, 1, 0.12, 0, 0.6, 0.5, 0.8, 400);
    LEADHI = syn_track("leadhi", syn_supersaw); SP(LEADHI, 7, 36, 0.3, 3, 10, 0.12, 0.003, 0, 1, 0.1, 0, 0.6, 1.0, 0.8, 2000);
    LEADSQ = syn_track("leadsq", syn_saw);    SP(LEADSQ, 3, 14, 2.0, 7, 0.06, 1.3, 0.25, 0.05, 0.9, 0.002);
    g_tr[LEAD].gain_db = -4; g_tr[LEADLO].gain_db = -6; g_tr[LEADHI].gain_db = -15; g_tr[LEADSQ].gain_db = -9;
    for (int t = LEAD; t <= LEADSQ; t++) { g_tr[t].group = 2; g_tr[t].send[0] = 0.12; g_tr[t].send[2] = 0.08; eq(t, 2, 150, 0.7, 0); }
    g_grp[2] = (Group){.cthr = -16, .cratio = 3, .catt = 0.004, .crel = 0.08, .drive = 0.4, .parallel = 0.6, .duck_db = 3, .duck_rel = 0.08,
                       .eq = {{3, 1600, 0.6, -5}}, .neq = 1};   // against LE SSERAFIM: 7 dB too much 0.8-3 kHz
    LEAD2 = syn_track("lead2", syn_saw);  SP(LEAD2, 3, 12, 1.5, 6, 0.08, 1.0, 0.3, 0.08, 0.4, 0.004); g_tr[LEAD2].gain_db = -1; g_tr[LEAD2].send[0] = 0.15; g_tr[LEAD2].send[2] = 0.12; g_tr[LEAD2].pan = -0.2; eq(LEAD2, 2, 200, 0.7, 0);
    ARP = syn_track("arp", syn_saw);      SP(ARP, 2, 8, 1.3, 10, 0.05, 1.2, 0.15, 0.04, 0.2, 0.001); g_tr[ARP].gain_db = -6; g_tr[ARP].send[0] = 0.2; g_tr[ARP].send[2] = 0.2; eq(ARP, 2, 150, 0.7, 0);
    g_tr[ARP].chorus_mix = 0.4; g_tr[ARP].chorus_ms = 7; g_tr[ARP].chorus_depth_ms = 1.5;
    STAB = syn_track("stab", syn_saw);    SP(STAB, 5, 16, 2.5, 10, 0.05, 0.8, 0.12, 0.05, 0, 0.001); g_tr[STAB].gain_db = -4; g_tr[STAB].send[0] = 0.2; eq(STAB, 2, 250, 0.7, 0);
    g_tr[STAB].chorus_mix = 0.5; g_tr[STAB].chorus_ms = 8; g_tr[STAB].chorus_depth_ms = 2;
    BELL = smp("bell", SM "vibes.txt", 1.0, 0); g_tr[BELL].gain_db = -2; g_tr[BELL].pan = 0.3; g_tr[BELL].send[1] = 0.3;
    PAD = syn_track("pad", syn_pad);      SP(PAD, 6, 10, 2.0, 0.4, 0.8, 0.6, 500, 0.3); g_tr[PAD].gain_db = -12; g_tr[PAD].send[1] = 0.3; eq(PAD, 2, 200, 0.7, 0);
    PIANO = smp("piano", SM "salamander.txt", 0.3, 0); g_tr[PIANO].sp[9] = 1; g_tr[PIANO].gain_db = -2; g_tr[PIANO].send[1] = 0.35; g_tr[PIANO].human_ms = 5;
    // the New York house sounds (round 2): bright M1 piano and the M1 Organ 2 bass recipe, swung
    M1P = syn_track("m1piano", syn_piano); SP(M1P, 1.9, 0.55, 3.5, 1.4, 0.7, 0.45, 0.07, 0.15, 15000, 0.11, 1.3);
    g_tr[M1P].gain_db = 4; g_tr[M1P].send[0] = 0.15; eq(M1P, 2, 180, 0.7, 0); eq(M1P, 3, 400, 1, -3); eq(M1P, 5, 2500, 0, 2);
    g_tr[M1P].cthr = -18; g_tr[M1P].cratio = 3; g_tr[M1P].catt = 0.004; g_tr[M1P].crel = 0.1; g_tr[M1P].drive = 0.3; g_tr[M1P].swing = 0.16;
    ORGAN = syn_track("organ", syn_organ); SP(ORGAN, 0.11, 0.45, 0.25, 0.9, 0.025, 0.8, 0.28, 2500, 3);
    g_tr[ORGAN].gain_db = -4; eq(ORGAN, 2, 38, 0.7, 0); eq(ORGAN, 3, 160, 1, 2); g_tr[ORGAN].swing = 0.16; g_tr[ORGAN].delay_ms = 10;
    B808 = syn_track("808", syn_808);     SP(B808, 0, 1.2, 5, 0.04, 0.1); g_tr[B808].gain_db = -3; eq(B808, 2, 30, 0.7, 0); eq(B808, 1, 1800, 0.7, 0);
    SUB = syn_track("sub", syn_sub);      SP(SUB, 0.01, 0.1, 0.1, 0.3, 0); g_tr[SUB].gain_db = -5; eq(SUB, 1, 110, 0.7, 0);
    VLN = smp("violins", SM "vln_sus.txt", 0.3, 1); g_tr[VLN].pan = -0.4; g_tr[VLN].delay_ms = -30; g_tr[VLN].gain_db = -2; g_tr[VLN].send[1] = 0.3;
    VLA = smp("violas", SM "vla_sus.txt", 0.3, 1);  g_tr[VLA].pan = 0.1; g_tr[VLA].delay_ms = -30; g_tr[VLA].gain_db = -4; g_tr[VLA].send[1] = 0.3;
    VC = smp("cellos", SM "vc_sus.txt", 0.3, 1);    g_tr[VC].pan = 0.4; g_tr[VC].delay_ms = -30; g_tr[VC].gain_db = -4; g_tr[VC].send[1] = 0.3;
    TPT = smp("trumpets", SM "tpt_stac.txt", 0.12, 0); g_tr[TPT].pan = 0.25; g_tr[TPT].gain_db = 0; g_tr[TPT].send[0] = 0.2;
    TBN = smp("trombones", SM "tbn_stac.txt", 0.12, 0); g_tr[TBN].pan = -0.25; g_tr[TBN].gain_db = -1; g_tr[TBN].send[0] = 0.2;
    HRN = smp("horns", SM "hn_stac.txt", 0.15, 0);  g_tr[HRN].pan = 0.0; g_tr[HRN].gain_db = -3; g_tr[HRN].send[0] = 0.2;
    // round 4: "too soundtrack/classical" - the orchestra steps back behind K-pop production layers. Strings are doubled by a
    // synth string ensemble, high-passed and pumped by the kick; the brass stabs become synth brass with the sampled brass under
    for (int t = VLN; t <= VC; t++) { g_tr[t].sp[12] = 1; g_tr[t].delay_ms = -10; g_tr[t].gain_db -= 6; eq(t, 2, 200, 0.7, 0); }
    for (int t = TPT; t <= HRN; t++) g_tr[t].gain_db -= 6;
    SSTR = syn_track("synth strings", syn_pad); SP(SSTR, 7, 14, 3.5, 0.08, 0.35, 0.5, 900, 0.0); g_tr[SSTR].gain_db = -9; g_tr[SSTR].send[1] = 0.25; eq(SSTR, 2, 220, 0.7, 0);
    g_tr[SSTR].chorus_mix = 0.35; g_tr[SSTR].chorus_ms = 9; g_tr[SSTR].chorus_depth_ms = 2;
    SBRS = syn_track("synth brass", syn_saw); SP(SBRS, 5, 14, 1.6, 6, 0.06, 0.9, 0.18, 0.06, 0.15, 0.004); g_tr[SBRS].gain_db = -5; g_tr[SBRS].send[0] = 0.18; eq(SBRS, 2, 180, 0.7, 0); eq(SBRS, 3, 1400, 1, 2);
    CHANT = syn_track("chant", syn_voice); SP(CHANT, 1, 1.0, 8, 25, 5, 0, 0.35, 0.004, 0.05, 0, 0, 0, 1, 0.03, 0.012, 4, 9000);
    g_tr[CHANT].gain_db = -8; g_tr[CHANT].send[0] = 0.3; eq(CHANT, 2, 220, 0.7, 0); eq(CHANT, 3, 2500, 1, 3);
    int duck[] = {PAD, ARP, STAB, LEAD2, B808, SSTR, SBRS, VLN, VLA, VC}; for (unsigned i = 0; i < 10; i++) { g_tr[duck[i]].duck_src = KICK; g_tr[duck[i]].duck_db = 3; g_tr[duck[i]].duck_rel = 0.08; }
    g_grp[2].duck_src = KICK;
    g_bus[0] = (Bus){.type = 1, .rev = {0.6, 0.35, 5, 0.6, 300, 9000, 0.15, 1.0, 0, 0}, .gain_db = -8, .duck_src = -1};
    g_bus[1] = (Bus){.type = 1, .rev = {2.2, 1.0, 25, 1.2, 250, 8000, 0.1, 1.0, 0.2, 0.2}, .gain_db = -7, .duck_src = -1};
    g_bus[2] = (Bus){.type = 2, .dl_s = 60.0 / 156 * 0.75, .dr_s = 60.0 / 156 * 0.75, .fb = 0.25, .lp = 5000, .hp = 600, .gain_db = -14, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 30, 0.7, 0}; g_master.eq[1] = (EqBand){3, 220, 0.8, -1.5}; g_master.eq[2] = (EqBand){5, 4500, 0, 7}; g_master.eq[3] = (EqBand){4, 90, 0, 4}; g_master.eq[4] = (EqBand){3, 1800, 0.7, -3}; g_master.neq = 5;
    g_master.cthr = -18; g_master.cratio = 3.5; g_master.catt = 0.005; g_master.crel = 0.1; g_master.width_lo_hz = 120;
    g_master.tape = 0.6; g_master.top_hz = 18000; g_dyn_depth = 0.3;   // a dense, compressed K-pop master (the reference's LRA is 2.6 LU)
}
static int map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    struct { const char *n; int *t; double dp; } m[] = {
        {"KICK", &KICK, 60}, {"CLAP", &CLAP, 60}, {"SNARE", &SNARE, 60}, {"GHOST", &GHOST, 60}, {"HAT", &HAT, 60}, {"OHAT", &OHAT, 60},
        {"SQUEAK", &SQK, 81}, {"SHAKER", &SHK, 60}, {"CRASH", &CRASH, -60}, {"IMPACT", &IMPACT, 36}, {"RISER", &RISER, -60}, {"SNAREROLL", &ROLL, 60},
        {"LEAD", &LEAD, 0}, {"LEAD2", &LEAD2, 0}, {"ARP", &ARP, 0}, {"STAB", &STAB, 0}, {"BELL", &BELL, 0}, {"PAD", &PAD, 0}, {"PIANO", &PIANO, 0},
        {"M1PIANO", &M1P, 0}, {"ORGAN", &ORGAN, 0}, {"BASS808", &B808, 0}, {"SUB", &SUB, 0}, {"STRINGS", &VLN, 0}, {"BRASS", &TPT, 0}, {"CHANT", &CHANT, 0}};
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(p, m[i].n)) { if (m[i].dp) *dp = m[i].dp; return *m[i].t; }
    return -1;
}
static void split_chords(int n0, int src, const int *dst, int nd) {
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { if (g_note[i].tr != src) continue;
        int idx[8], c = 0; for (int j = i; j < n1 && c < 8; j++) if (g_note[j].tr == src && fabs(g_note[j].beat - g_note[i].beat) < 1e-6) idx[c++] = j;
        for (int a = 0; a < c; a++) for (int b = a + 1; b < c; b++) if (g_note[idx[b]].pitch < g_note[idx[a]].pitch) { int x = idx[a]; idx[a] = idx[b]; idx[b] = x; }
        for (int a = 0; a < c; a++) { int k = c == 1 ? nd - 1 : (int)lround((double)a * (nd - 1) / (c - 1)); g_note[idx[a]].tr = -dst[k] - 2; } }
    for (int i = n0; i < n1; i++) if (g_note[i].tr < -1) g_note[i].tr = -g_note[i].tr - 2;
}
static int in_sec(double beat, const char *prefix) { for (int s = 0; s < g_nsec; s++) if (!strncmp(g_sec[s].name, prefix, strlen(prefix)) && beat >= g_sec[s].bar0 * 4.0 && beat < (g_sec[s].bar0 + g_sec[s].bars) * 4.0) return 1; return 0; }
static void post(int n0) {
    { int n1 = g_nnote; for (int i = n0; i < n1; i++) { Note a = g_note[i];
        if (a.tr == VLN) note(SSTR, a.beat, a.len, a.pitch, a.vel * 0.9);
        if (a.tr == TPT) note(SBRS, a.beat, fmin(a.len, 0.45), a.pitch, a.vel); } }
    int sd[3] = {VC, VLA, VLN}; split_chords(n0, VLN, sd, 3);
    int bd[3] = {TBN, HRN, TPT}; split_chords(n0, TPT, bd, 3);
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { Note n = g_note[i];
        if (n.tr == LEAD) { // the stack: the full chord on the main layer; the top note an octave up; the bottom note an octave down; a square on the top note
            int top = 1, bot = 1; for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == LEAD && fabs(g_note[j].beat - n.beat) < 1e-6) { if (g_note[j].pitch > n.pitch) top = 0; if (g_note[j].pitch < n.pitch) bot = 0; }
            if (top) { note(LEADHI, n.beat, n.len, n.pitch + 12, n.vel * 0.8); note(LEADSQ, n.beat, n.len, n.pitch, n.vel * 0.9); }
            if (bot) note(LEADLO, n.beat, n.len, n.pitch - 12, n.vel * 0.9); }
        if (n.tr == KICK) note(KTOP, n.beat, 0.05, 60, n.vel);
        if (n.tr == SUB && n.pitch < 40) g_note[i].pitch += 12;
        if (n.tr == CHANT) { g_note[i].len = fmin(n.len, 0.3); g_note[i].p[0] = 1; g_note[i].p[1] = 2; g_note[i].p[2] = 2.5; }
        // the house part swings its 16ths (58%), as New York did
        if ((n.tr == HAT || n.tr == SHK || n.tr == OHAT) && in_sec(n.beat, "Itaewon")) { double s16 = n.beat * 4; if (fabs(s16 - lround(s16)) < 1e-6 && (lround(s16) & 1)) g_note[i].beat += 0.04; }
        if (n.tr == OHAT) g_note[i].len = 0.4;   // choked by the next closed hat
    }
    // chant: three voices an octave apart
    for (int i = n0; i < n1; i++) if (g_note[i].tr == CHANT) { Note n = g_note[i]; Note *a = note(CHANT, n.beat, n.len, n.pitch + 7, n.vel); *a = n; a->pitch += 7; a->p[3] = 0.5; Note *b = note(CHANT, n.beat, n.len, n.pitch + 12, n.vel); *b = n; b->pitch += 12; b->p[3] = -0.5; }
}
static void automation(void) {
    // the piece ends with these filters open, so the wrap point holds them open until the intro closes them
    autolp(PAD, -0.01, 19000); autolp(ARP, -0.01, 19000); autolp(ARP, 0, 600);
    sec_lp(ARP, "Gangnam Girl-Crush: Intro", 5, 8, 600, 16000, 0); sec_lp(PAD, "Gangnam Girl-Crush: Intro", 1, 8, 500, 8000, 0);
}
double city_loop(void) { int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", map, 0); post(n0); automation(); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
