// Miami - tropical house, 100 BPM, A major, 4/4. Original material. Round 2: rebuilt on the production research
// (Kygo "Stole the Show" breakdown on delamar.de, Splice, EDMProd, soundcy, audeobox): see notes.md.
// Loop body (32 bars, 76.8 s): drop A (pan-flute hook, 8) + drop B (vocal-chop hook, 8) + break (8) + build (8) -> wraps into the drop.
#include "../engine/render.h"

#define BPM 100
static int KICK, CLAP, CLAP2, SNAP, SHK, TAMB, HAT, OHAT, BASS, PAD, PIANO, PLUCK, FLUTE, FLUTE8, STEEL, CHOP, OOH, RISER, REVC;
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)

void city_setup(void) {
    tempo(0, BPM);
    double spb = 60.0 / BPM;
    // drums: a round, soft kick tuned to the key (A1, 55 Hz), quiet in the mix; claps layered and spread; a snap on 2 and 4
    // into a long dark "cathedral" (0 pre-delay); tambourine on the off-beats; shaker 8ths in a small room;
    // a closed hat on the beat; one open hat every two bars on the and of 4. Light swing on the small percussion.
    KICK = syn_track("kick", syn_kick);   SP(KICK, 55, 110, 0.03, 0.12, 0.0, 0.6, 0.18, 0.08);
    g_tr[KICK].gain_db = -6; eq(KICK, 1, 1800, 0.7, 0); eq(KICK, 2, 30, 0.7, 0); g_tr[KICK].group = 1;
    CLAP = syn_track("clap", syn_clap);   SP(CLAP, 1300, 1.2, 0.08, 9, 3, 700);
    g_tr[CLAP].gain_db = -15; g_tr[CLAP].pan = -0.12; g_tr[CLAP].send[0] = 0.25; g_tr[CLAP].group = 1;
    CLAP2 = syn_track("clap2", syn_clap); SP(CLAP2, 1700, 1.4, 0.06, 7, 3, 900);
    g_tr[CLAP2].gain_db = -18; g_tr[CLAP2].pan = 0.12; g_tr[CLAP2].delay_ms = 6; g_tr[CLAP2].send[0] = 0.25; g_tr[CLAP2].group = 1;
    SNAP = syn_track("snap", syn_rim);    SP(SNAP, 2400, 1200, 0.010);
    g_tr[SNAP].gain_db = -12; g_tr[SNAP].send[3] = 0.55; g_tr[SNAP].group = 1;
    SHK = syn_track("shaker", syn_shaker); SP(SHK, 7000, 0.9, 0.014, 0.04);
    g_tr[SHK].gain_db = -16; g_tr[SHK].pan = -0.3; g_tr[SHK].human_ms = 3; g_tr[SHK].human_vel = 0.12; g_tr[SHK].swing = 0.08; g_tr[SHK].send[0] = 0.2; g_tr[SHK].group = 1;
    TAMB = syn_track("tamb", syn_hat);    SP(TAMB, 0.09, 3.1, 0.7, 6500, 9000);
    g_tr[TAMB].gain_db = -18; g_tr[TAMB].pan = 0.3; g_tr[TAMB].human_ms = 2; g_tr[TAMB].human_vel = 0.1; g_tr[TAMB].send[0] = 0.15; g_tr[TAMB].group = 1;
    HAT = syn_track("hat", syn_hat);      SP(HAT, 0.025, 1.9, 0.5, 8000, 11000);
    g_tr[HAT].gain_db = -21; g_tr[HAT].pan = 0.15; g_tr[HAT].send[0] = 0.2; g_tr[HAT].group = 1;
    OHAT = syn_track("ohat", syn_hat);    SP(OHAT, 0.2, 1.9, 0.5, 7500, 10000);
    g_tr[OHAT].gain_db = -24; g_tr[OHAT].pan = 0.15; g_tr[OHAT].group = 1;
    g_grp[1] = (Group){.cthr = -20, .cratio = 4, .catt = 0.01, .crel = 0.12, .drive = 0.3, .parallel = 0.5, .gain_db = 1};
    // sub bass only in the drops: soft, high-passed at 30 Hz, shelved off above ~90 Hz; ducked hard and fast by the kick
    BASS = syn_track("bass", syn_sub);    SP(BASS, 0.008, 0.08, 0.12, 0.3, 0);
    g_tr[BASS].gain_db = -9; eq(BASS, 2, 30, 0.7, 0); eq(BASS, 5, 110, 0, -6);
    g_tr[BASS].duck_src = KICK; g_tr[BASS].duck_db = 10; g_tr[BASS].duck_rel = 0.16; g_tr[BASS].duck_att = 0.003;
    // warm pad an octave over the piano: slow attack, detuned, chorused, wide, slower sidechain release
    PAD = syn_track("pad", syn_pad);      SP(PAD, 6, 12, 2.4, 0.9, 0.5, 0.6, 800, 0);
    g_tr[PAD].gain_db = -17; g_tr[PAD].duck_src = KICK; g_tr[PAD].duck_db = 8; g_tr[PAD].duck_rel = 0.38; g_tr[PAD].send[1] = 0.25;
    g_tr[PAD].chorus_mix = 0.5; g_tr[PAD].chorus_ms = 9; g_tr[PAD].chorus_depth_ms = 2.5; g_tr[PAD].chorus_hz = 0.4;
    eq(PAD, 2, 250, 0.7, 0); eq(PAD, 3, 400, 1, -3); eq(PAD, 5, 8000, 0, 2);
    // piano: warm, soft hammer, the bed of the verse and the break; every other note softer
    PIANO = syn_track("piano", syn_piano); SP(PIANO, 0.85, 0.9, 0.6, 0.6, 0.6, 0.5, 0.15, 0.25, 12000, 0.125, 1.6);
    g_tr[PIANO].gain_db = -1; g_tr[PIANO].send[1] = 0.25; g_tr[PIANO].human_ms = 5; g_tr[PIANO].duck_src = KICK; g_tr[PIANO].duck_db = 3; g_tr[PIANO].duck_rel = 0.3;
    eq(PIANO, 2, 120, 0.7, 0); eq(PIANO, 3, 300, 0.9, -3);
    // marimba-style pluck: amp and filter share one envelope (A 0, D 240 ms, S ~28%, R 90 ms)
    PLUCK = syn_track("pluck", syn_mallet); SP(PLUCK, 0.24, 0.8, 0.8, 0.24, 0.12, 0, 7);
    g_tr[PLUCK].gain_db = -9; g_tr[PLUCK].send[0] = 0.2; g_tr[PLUCK].send[2] = 0.22; eq(PLUCK, 2, 300, 0.7, 0); eq(PLUCK, 5, 3000, 0, 3);
    g_tr[PLUCK].duck_src = KICK; g_tr[PLUCK].duck_db = 3; g_tr[PLUCK].duck_rel = 0.2;
    // pan pipe lead (stopped pipe: odd harmonics, breathy, chiff, slides between notes) and its octave double
    FLUTE = syn_track("panflute", syn_flute); SP(FLUTE, 0.42, 12, 0.03, 0.04, 0.16, 0.07, 0.8, 0.05);
    g_tr[FLUTE].gain_db = -4; g_tr[FLUTE].send[1] = 0.4; g_tr[FLUTE].send[2] = 0.18; eq(FLUTE, 2, 350, 0.7, 0); eq(FLUTE, 5, 6000, 0, 2);
    FLUTE8 = syn_track("panflute8", syn_flute); SP(FLUTE8, 0.25, 10, 0.03, 0.05, 0.12, 0.07, 0.5, 0.03);
    g_tr[FLUTE8].gain_db = -12; g_tr[FLUTE8].send[1] = 0.4; g_tr[FLUTE8].pan = 0; eq(FLUTE8, 2, 200, 0.7, 0);
    STEEL = syn_track("steel", syn_steelpan); SP(STEEL, 0.55, 0.6, 0.2, 0.25);
    g_tr[STEEL].gain_db = -8; g_tr[STEEL].pan = 0.1; g_tr[STEEL].send[1] = 0.3; g_tr[STEEL].send[2] = 0.2; eq(STEEL, 2, 300, 0.7, 0);
    // vocal chops: 1/8-1/16 fragments, formant up, two panned layers; short room (0.8 s, 12 ms pre-delay), high cut 7.5 kHz, dotted-8th delay
    CHOP = syn_track("chop", syn_voice);  SP(CHOP, 3, 1.18, 2, 5, 5.5, 0, 0.07, 0.005, 0.04, -1, 0, 0, 1, 0.025, 0.004, 3, 8000);
    g_tr[CHOP].gain_db = -6; g_tr[CHOP].send[0] = 0.35; g_tr[CHOP].send[2] = 0.3; eq(CHOP, 2, 300, 0.7, 0); eq(CHOP, 1, 7500, 0.7, 0); eq(CHOP, 3, 3000, 1, 2);
    g_tr[CHOP].duck_src = KICK; g_tr[CHOP].duck_db = 2; g_tr[CHOP].duck_rel = 0.12;
    OOH = syn_track("ooh", syn_voice);    SP(OOH, 3, 1.0, 5, 9, 5.2, 18, 0.1, 0.3, 0.6, 1, 0, 0, 0.4, 0.1, 0.05, 0, 7000);
    g_tr[OOH].gain_db = -17; g_tr[OOH].send[1] = 0.5; eq(OOH, 2, 300, 0.7, 0); eq(OOH, 1, 7000, 0.7, 0);
    RISER = syn_track("riser", syn_riser); SP(RISER, 400, 9000, 2.5);
    g_tr[RISER].gain_db = -22; g_tr[RISER].send[1] = 0.4;
    REVC = syn_track("revcrash", syn_riser); SP(REVC, 3000, 12000, 1.0);
    g_tr[REVC].gain_db = -20; g_tr[REVC].send[1] = 0.3;
    // buses: small room/plate (0.9 s), hall (1.9 s, kept under 2 s), dotted-8th ping-pong, a long dark cathedral for the snap only
    g_bus[0] = (Bus){.type = 1, .rev = {0.9, 0.5, 12, 0.7, 300, 7500, 0.15, 1.0, 0.3, 0.2}, .gain_db = -4, .duck_src = -1};
    g_bus[1] = (Bus){.type = 1, .rev = {1.9, 0.9, 25, 1.1, 250, 7000, 0.10, 1.0, 0.25, 0.25}, .gain_db = -4, .duck_src = KICK, .duck_db = 3, .duck_rel = 0.3};
    g_bus[2] = (Bus){.type = 2, .dl_s = spb * 0.75, .dr_s = spb * 0.75, .fb = 0.35, .lp = 4500, .hp = 500, .gain_db = -9, .duck_src = -1};
    g_bus[3] = (Bus){.type = 1, .rev = {3.6, 1.2, 0, 1.5, 400, 6000, 0.05, 1.0, 0.2, 0.3}, .gain_db = -6, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 28, 0.7, 0}; g_master.eq[1] = (EqBand){3, 350, 0.8, -1.5}; g_master.eq[2] = (EqBand){5, 8000, 0, 3}; g_master.eq[3] = (EqBand){3, 3500, 0.8, 2}; g_master.neq = 4;
    g_master.cthr = -14; g_master.cratio = 1.8; g_master.catt = 0.02; g_master.crel = 0.2; g_master.width_lo_hz = 140;
}

// ---- harmony: F#m9 - Dmaj9 - Aadd9 - E9 (vi-IV-I-V; the V is a dominant 9th) ----
static const char *PIA[4] = {"A3+C#4+E4+G#4", "F#3+A3+C#4+E4", "E3+G#3+C#4+E4", "G#3+D4+F#4+B4"};   // F#m7(9) - Dmaj9 - Amaj7 - E9 (Fable)
static const char *PADV[4] = {"C#4+E4+G#4", "C#4+E4+F#4", "C#4+E4+G#4", "D4+F#4+G#4"};
static const double ROOT[4] = {42, 38, 45, 40};   // F#2 D2 A2 E2

static void piano_bar(double b, int ch, double v, int sparse) {
    double ps[8]; int np = chord_pitches(PIA[ch], ps);
    static const double hits[3] = {0, 1.5, 2.5}, lens[3] = {1.5, 1.0, 1.5};
    for (int h = 0; h < 3; h++) { if (sparse && h == 1) continue;
        for (int i = 0; i < np; i++) note(PIANO, b + hits[h] + i * 0.006, lens[h], ps[i], v * (h == 1 ? 0.7 : h == 2 ? 0.85 : 1) * (i == 0 ? 0.9 : 1)); }
    note(PIANO, b, 3.6, ROOT[ch] + 12, v * 0.8);
    cc(PIANO, b + 0.05, 64, 127); cc(PIANO, b + 3.9, 64, 0);
}
static void pad_bar(double b, int ch, double v) { double ps[8]; int np = chord_pitches(PADV[ch], ps); for (int i = 0; i < np; i++) note(PAD, b, 4.0, ps[i] + 12, v); }
static void bass_bar(double b, int ch, double v) {
    double r = ROOT[ch];
    note(BASS, b, 0.9, r, v); note(BASS, b + 1.5, 0.4, r, v * 0.7); note(BASS, b + 2, 0.9, r, v * 0.9); note(BASS, b + 3.5, 0.4, r + 7, v * 0.7);
}
static void drums_bar(double b, int lvl, double v) {   // lvl 0 perc only, 1 + kick, 2 full drop kit
    if (lvl >= 1) for (int i = 0; i < 4; i++) note(KICK, b + i, 0.2, 45, v);
    pat(SHK, b, 0.5, "xxxxxxxx", 60, v * 0.55);
    if (lvl >= 2) {
        for (int i = 0; i < 2; i++) { double c = b + 1 + 2 * i; note(CLAP, c, 0.2, 60, v * 0.9); note(CLAP2, c, 0.2, 60, v * 0.8); note(SNAP, c, 0.1, 60, v * 0.85); }
        for (int i = 0; i < 4; i++) { note(TAMB, b + i + 0.5, 0.1, 60, v * (i & 1 ? 0.55 : 0.7)); note(HAT, b + i, 0.1, 60, v * 0.5); }
    }
}
// the hook, 8 bars (3-3-2-2-2-2-2 sixteenth placements); slides on stepwise 16ths
static const char *HOOK[8] = {   // drop A, pan flute (Fable): a lazy E-F#-A climb landing late, answered by C#-D-E
    "r:0.5 E5:0.5 F#5:0.5 A5:1 F#5:0.5 E5:1",
    "C#5:1.5 D5:0.5 E5:1 r:1",
    "r:0.5 E5:0.5 G#5:0.5 A5:1.5 E5:1",
    "F#5:0.5 E5:0.5 D5:0.5 B4:1 r:1.5",
    "r:0.5 E5:0.5 F#5:0.5 A5:1 F#5:0.5 E5:0.5 C#5:0.5",
    "D5:1 E5:0.5 F#5:1 E5:0.5 D5:1",
    "r:0.5 C#5:0.5 E5:0.5 A5:1.5 G#5:0.5 F#5:0.5",
    "F#5:0.5 E5:0.5 B4:0.5 A4:1 r:0.5 E5:1"};
static const char *DROPB[8] = {   // drop B, steel pan (Fable): the same climb re-cut into 16th pairs
    "E5:0.25 F#5:0.25 A5:0.5 F#5:0.25 E5:0.25 C#5:0.5 E5:0.5 F#5:0.25 A5:0.25 F#5:0.5 E5:0.5",
    "D5:0.5 C#5:0.25 D5:0.25 E5:0.5 F#5:0.25 E5:0.25 D5:0.5 A4:0.5 C#5:0.25 D5:0.25 E5:0.5",
    "E5:0.25 G#5:0.25 A5:0.5 G#5:0.25 E5:0.25 C#5:0.5 E5:0.5 G#5:0.25 A5:0.25 B5:0.5 A5:0.5",
    "F#5:0.5 E5:0.25 D5:0.25 B4:0.5 D5:0.25 E5:0.25 F#5:0.5 G#5:0.5 F#5:0.25 E5:0.25 D5:0.5",
    "E5:0.25 F#5:0.25 A5:0.5 F#5:0.25 E5:0.25 C#5:0.5 E5:0.5 A5:0.25 C#6:0.25 A5:0.5 F#5:0.5",
    "D5:0.5 F#5:0.25 A5:0.25 F#5:0.5 D5:0.25 E5:0.25 F#5:0.5 A5:0.5 F#5:0.25 E5:0.25 D5:0.5",
    "C#5:0.25 E5:0.25 A5:0.5 E5:0.25 C#5:0.25 A4:0.5 C#5:0.5 E5:0.25 G#5:0.25 A5:0.5 C#6:0.5",
    "B5:0.5 A5:0.25 G#5:0.25 F#5:0.5 E5:0.25 D5:0.25 B4:0.5 r:0.5 E5:0.25 F#5:0.25 G#5:0.5"};
static void flute_line(int tr, double b, const char *m, double v, double tp) {
    int n0 = g_nnote; double sg = g_gate; g_gate = 0.97; mel(tr, b, m, v, tp); g_gate = sg;
    for (int i = n0 + 1; i < g_nnote; i++) { Note *a = &g_note[i - 1], *n = &g_note[i];
        if (fabs(a->beat + a->len / 0.97 - n->beat) < 0.01 && fabs(a->pitch - n->pitch) <= 3) n->p[2] = a->pitch - n->pitch; }
}
static void chop_bar(double b, int bar, double v) {
    int n0 = g_nnote; mel(CHOP, b, HOOK[bar], v, 12);
    static const double vw[7][2] = {{0, 1.8}, {3, -1}, {0, -1}, {1, 2}, {3, 4}, {0, 1.8}, {3, -1}};
    for (int i = n0, j = 0; i < g_nnote; i++, j++) { Note *n = &g_note[i]; n->len = fmin(n->len, 0.4); n->p[0] = vw[j % 7][0]; n->p[1] = vw[j % 7][1]; n->p[3] = (j & 1) ? 0.3 : -0.3; }
}

static void drop(double b0, int chops) {
    for (int i = 0; i < 8; i++) {
        double b = b0 + i * 4; int ch = i % 4;
        drums_bar(b, 2, 0.95);
        if (i % 2 == 1) note(OHAT, b + 3.5, 0.2, 60, 0.6);
        bass_bar(b, ch, 0.9); pad_bar(b, ch, 0.5); piano_bar(b, ch, 0.42, 1);
        if (!chops) { flute_line(FLUTE, b, HOOK[i], 0.85, 0); flute_line(FLUTE8, b, HOOK[i], 0.7, -12); mel(PLUCK, b, HOOK[i], 0.55, -12); }
        else {
            // drop B: the steel pan takes the hook with the marimba an octave under; the pan pipe answers at phrase ends
            mel(STEEL, b, DROPB[i], 0.8, 0); mel(PLUCK, b, DROPB[i], 0.45, -12);
            if (i % 2 == 1) flute_line(FLUTE8, b + 2, "E5:0.5 F#5:0.5 E5:1", 0.5, 0);
            double ps[8]; int np = chord_pitches(PADV[ch], ps);
            for (int s = 0; s < 8; s++) { Note *n = note(PLUCK, b + s * 0.5, 0.4, ps[(s * 3) % np] + 12, (s & 1) ? 0.25 : 0.32); n->p[3] = (s & 1) ? 0.5 : -0.5; }
        }
        if (!chops && (i == 3 || i == 7)) mel(STEEL, b + 3, "E6:0.25 C#6:0.25 B5:0.5", 0.6, 0);
    }
}
static const char *FLMEL[8] = {"A4:2 C#5:1 E5:1", "F#5:3 E5:1", "D5:2 E5:1 F#5:1", "E5:4",
                               "A4:2 C#5:1 E5:1", "A5:3 G#5:1", "F#5:1.5 E5:0.5 D5:2", "E5:3 r:1"};   // break (Fable)
static void brk(double b0) {   // piano and pad carry it; the pan pipe sings slowly; no kick, no bass until the second half
    for (int i = 0; i < 8; i++) {
        double b = b0 + i * 4; int ch = i % 4;
        pad_bar(b, ch, 0.55); piano_bar(b, ch, 0.6, 0);
        flute_line(FLUTE, b, FLMEL[i], 0.75, 0);
        drums_bar(b, 0, i < 4 ? 0.55 : 0.75);
        if (i >= 4) { note(SNAP, b + 1, 0.1, 60, 0.6); note(SNAP, b + 3, 0.1, 60, 0.6); }
        if (i == 1) mel(STEEL, b + 2.5, "A5:0.25 B5:0.25 C#6:0.5 E6:0.5", 0.55, 0);
        if (i == 3) mel(STEEL, b + 3, "B5:0.25 G#5:0.25 E5:0.5", 0.55, 0);
        if (i == 5) mel(STEEL, b + 2.5, "C#6:0.25 B5:0.25 A5:0.5 F#5:0.5", 0.55, 0);
    }
    note(KICK, b0, 0.2, 45, 0.8);
}
static void build(double b0) {   // automation builds it: the piano opens from dark to bright, the kick comes up, reverb grows
    for (int i = 0; i < 8; i++) {
        double b = b0 + i * 4; int ch = i % 4;
        drums_bar(b, i >= 2 ? 1 : 0, 0.55 + 0.05 * i);
        if (i >= 4) for (int k = 0; k < 2; k++) note(CLAP, b + 1 + 2 * k, 0.2, 60, 0.5 + 0.05 * i);
        pad_bar(b, ch, 0.45 + 0.03 * i); piano_bar(b, ch, 0.55 + 0.02 * i, 0);
        mel(PLUCK, b, HOOK[i % 4], 0.45 + 0.04 * i, 0);
    }
    autolp(PIANO, b0, 600); autolp(PIANO, b0 + 28, 16000); autolp(PLUCK, b0, 900); autolp(PLUCK, b0 + 30, 16000); autolp(PLUCK, b0 + 32, 19000); autolp(PIANO, b0 + 32, 19000);
    for (int s = 0; s < 8; s++) note(CLAP, b0 + 24 + s * 0.5, 0.1, 60, 0.35 + 0.04 * s);
    for (int s = 0; s < 12; s++) note(CLAP, b0 + 28 + s * 0.25, 0.1, 60, 0.6 + 0.03 * s);
    note(RISER, b0 + 16, 16, 60, 0.8);
    Note *rc = note(REVC, b0 + 30, 2, 60, 0.9); rc->p[0] = 1;
    // one beat of air before the drop
    for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i]; if (n->k != g_k) continue; if (n->tr == RISER || n->tr == REVC) continue;
        if (n->beat >= b0 + 31 && n->beat < b0 + 32) n->vel = 0.0001;
        else if (n->beat < b0 + 31 && n->beat + n->len > b0 + 31 && n->tr != KICK) n->len = b0 + 31 - n->beat; }
    cc(PIANO, b0 + 30.9, 64, 0);
}
static const char *VERSE[8] = {"r:1 C#5:0.5 C#5:0.5 E5:1 C#5:1", "B4:1 A4:1 B4:2", "r:1 C#5:0.5 E5:0.5 F#5:1 E5:1", "B4:4",
                               "r:1 C#5:0.5 C#5:0.5 E5:1 F#5:1", "A5:1 F#5:1 E5:2", "r:1 E5:0.5 F#5:0.5 E5:1 C#5:1", "B4:3 r:1"};
static void intro(double b0) {
    for (int i = 0; i < 8; i++) { double b = b0 + i * 4; int ch = i % 4; pad_bar(b, ch, 0.55); piano_bar(b, ch, 0.7, i < 2); if (i >= 2) drums_bar(b, 0, 0.4 + 0.04 * i);
        if (i >= 4) flute_line(FLUTE, b, FLMEL[i - 4], 0.6, 0); }
    Note *rc = note(REVC, b0 + 30, 2, 60, 0.7); rc->p[0] = 1;
}
static void verse(double b0) {
    for (int i = 0; i < 8; i++) { double b = b0 + i * 4; int ch = i % 4;
        drums_bar(b, 1, 0.75); if (i >= 4) { note(SNAP, b + 1, 0.1, 60, 0.6); note(SNAP, b + 3, 0.1, 60, 0.6); note(HAT, b + 2, 0.1, 60, 0.4); }
        pad_bar(b, ch, 0.4); piano_bar(b, ch, 0.6, 0);
        if (i >= 4) bass_bar(b, ch, 0.6);
        flute_line(FLUTE, b, FLMEL[i], 0.55, 0); }
}
static void outro(double b0) {
    for (int i = 0; i < 8; i++) { double b = b0 + i * 4; int ch = i % 4;
        if (i < 4) { drums_bar(b, 1, 0.8 - 0.05 * i); bass_bar(b, ch, 0.7); } else drums_bar(b, 0, 0.5 - 0.05 * i);
        pad_bar(b, ch, 0.55 - 0.02 * i); piano_bar(b, ch, 0.7 - 0.03 * i, i >= 6);
        if (i >= 4 && i < 7) flute_line(FLUTE, b, FLMEL[i - 4], 0.55, 0); }
    double ps[8]; int np = chord_pitches("A3+C#4+E4+B4", ps); for (int k = 0; k < np; k++) note(PIANO, b0 + 32, 6, ps[k], 0.65);
    note(PIANO, b0 + 32, 6, 45, 0.6); cc(PIANO, b0 + 32.05, 64, 127); pad_bar(b0 + 32, 2, 0.45); mel(STEEL, b0 + 32, "E6:1 C#6:1 A5:4", 0.5, 0);
}

void city_full(void) {
    double b = 0;
    intro(b); b += 32; verse(b); b += 32; build(b); b += 32;
    drop(b, 0); drop(b + 32, 1); b += 64; brk(b); b += 32; build(b); b += 32;
    drop(b, 0); drop(b + 32, 1); b += 64; outro(b);
}
double city_loop(void) { drop(0, 0); drop(32, 1); brk(64); build(96); return 128; }
