// Madrid - a salsa suite (round 3). Score text by Fable (score.txt): bolero, son pivot, salsa intro and montuno, charanga
// cha-cha-cha with a conga break, a G major mambo and mona, a piano descarga and final montuno, and the bolero reprise.
// Real percussion from VCSL: conga (muted, slap, open, tumba strokes picked by pitch), bongo (high and low), cowbells (bongo
// bell and the timbale mambo bell), claves, guiro (short and long scrapes), shaker; timbales are the bongo heads tuned down,
// the cascara the second claves pitched down. Horns are VSCO trumpets and trombones, each line doubled into a section;
// the solo trumpet plays the vibrato samples alone. Salamander piano, VCSL vibraphone and tenor sax, VSCO flute and violins,
// the double-bass pizzicato as the baby bass, and the tres as two plucked strings an octave apart (its doubled courses).
#include "../engine/orch.h"
#define SP(t, ...) do { double v_[] = {__VA_ARGS__}; memcpy(g_tr[t].sp, v_, sizeof v_); } while (0)

static int TRES2, GS, GL, GUIRO, CG, BG, CGS, BGS, PN, TR, VB, BS, BSUB;

void city_setup(void) {
    g_on_done = sections_out;
    // percussion: pitch picks the stroke, nothing is repitched
    int cg = o_smp("conga", OM "latin_conga.txt", 0.3, 0, 0.3, 0, 0.12, 8), bg = o_smp("bongo", OM "latin_bongo.txt", 0.3, 0, -0.3, 0, 0.12, 2);
    int tm = o_smp("timbales", OM "latin_timbal.txt", 0.3, 0, 0.35, 0, 0.15, 3); g_tr[tm].sp[6] = -500; eq(tm, 3, 3500, 1, 3);
    int bl = o_smp("bongo bell", OM "latin_bell.txt", 0.3, 0, -0.25, 7, 0.12, -2), tb = o_smp("mambo bell", OM "latin_tbell.txt", 0.3, 0, 0.35, 6, 0.12, -3);
    int cl = o_smp("clave", OM "latin_clave.txt", 0.2, 0, 0.45, 0, 0.12, 14), cs = o_smp("cascara", OM "latin_shell.txt", 0.2, 0, 0.35, 0, 0.1, 10); g_tr[cs].sp[6] = -700;
    GS = o_smp("guiro", OM "latin_guiro_s.txt", 0.2, 0, -0.45, 0, 0.12, 24); GL = o_smp("guiro long", OM "latin_guiro_l.txt", 0.2, 0, -0.45, 0, 0.12, 24);
    int mr = o_smp("maracas", OM "vcsl_shaker.txt", 0.2, 0, -0.4, 0, 0.12, 11), cr = o_smp("crash", OM "crash_oh.txt", 2.0, 0, 0.25, 0, 0.2, -6);
    for (int t = cg; t <= cr; t++) { g_tr[t].human_ms = 3; g_tr[t].human_vel = 0.06; }
    // the conga break: the soloing drums step forward (the band is out, so the room hears them louder and closer)
    CG = cg; BG = bg; CGS = o_smp("conga solo", OM "latin_conga.txt", 0.3, 0, 0.15, 0, 0.1, 15); BGS = o_smp("bongo solo", OM "latin_bongo.txt", 0.3, 0, -0.2, 0, 0.1, 10);
    g_tr[CGS].human_ms = g_tr[BGS].human_ms = 4; g_tr[CGS].human_vel = g_tr[BGS].human_vel = 0.08;
    // rhythm section
    int pn = PN = o_smp("piano", OM "salamander.txt", 0.25, 0, -0.3, 0, 0.12, -4); eq(pn, 3, 300, 1, -2); eq(pn, 5, 3000, 0, 2);
    int bs = BS = o_smp("bass", OM "dbass_pizz.txt", 0.4, 0, 0.0, -8, 0.06, 6); eq(bs, 3, 900, 1, 2);
    // the baby bass's fundamental: a sine under the upright (the low end measured -30 dB under 60 Hz)
    BSUB = syn_track("bass sub", syn_sub); SP(BSUB, 0.005, 0.12, 0.1, 0.3, 0.6); g_tr[BSUB].gain_db = -8; g_tr[BSUB].delay_ms = -8; eq(BSUB, 1, 140, 0.7, 0);
    int tr = TR = syn_track("tres", syn_guitar); SP(tr, 0.995, 0.75, 0.12, 0.5, 0.0); g_tr[tr].pan = -0.45; g_tr[tr].send[0] = 0.15; g_tr[tr].human_ms = 4; g_tr[tr].gain_db = 0;
    TRES2 = syn_track("tres octave", syn_guitar); SP(TRES2, 0.994, 0.8, 0.1, 0.4, 0.0); g_tr[TRES2].pan = -0.35; g_tr[TRES2].delay_ms = 9; g_tr[TRES2].send[0] = 0.15; g_tr[TRES2].gain_db = -6;
    eq(tr, 2, 120, 0.7, 0); eq(TRES2, 2, 200, 0.7, 0);
    int vb = VB = o_smp("vibes", OM "vibes.txt", 1.0, 0, 0.2, 0, 0.3, 2);
    // horns: each line a doubled section; the solo trumpet alone, with vibrato
    int t1 = o_smp("trumpet1", OM "tpt_sus.txt", 0.15, 1, 0.2, -15, 0.2, 0), t1b = o_double(t1, "trumpet1b", 0.1);
    int t2 = o_smp("trumpet2", OM "tpt_sus.txt", 0.15, 1, 0.4, -15, 0.2, -2), t2b = o_double(t2, "trumpet2b", 0.1);
    int b1 = o_smp("trombone1", OM "tbn_sus.txt", 0.15, 1, -0.2, -20, 0.2, 12), b1b = o_double(b1, "trombone1b", -0.1);
    int b2 = o_smp("trombone2", OM "tbn_sus.txt", 0.15, 1, -0.4, -20, 0.2, 14), b2b = o_double(b2, "trombone2b", -0.1);
    int so = o_smp("solo trumpet", OM "tpt_vib.txt", 0.2, 1, 0.0, -10, 0.28, 2);
    int sx = o_smp("sax", OM "tsax.txt", 0.2, 1, 0.1, -15, 0.25, 4), fl = o_smp("flute", OM "fl_vib.txt", 0.2, 1, -0.1, -15, 0.3, 0);
    int st = o_smp("strings", OM "vln_sus.txt", 0.3, 1, -0.5, -30, 0.35, 0), st2 = o_double(st, "strings2", 0.2); o_hi(st, "strings hi", 88);
    for (int t = t1; t <= so; t++) { eq(t, 2, 150, 0.7, 0); eq(t, 3, 2800, 1, -1); }
    o_part("CONGA", cg, -1, 0, 0, 0); o_part("BONGO", bg, -1, 0, 0, 0); o_part("TIMBALES", tm, -1, 0, 0, 0);
    o_part("BELL", bl, -1, 0, 1, 0); o_part("TBELL", tb, -1, 0, 1, 0); o_part("CLAVE", cl, -1, 0, 1, 0); o_part("CASCARA", cs, -1, 0, 1, 0);
    o_part("GUIRO", GS, -1, 0, 1, -60); o_part("MARACAS", mr, -1, 0, 1, 0); o_part("CRASH", cr, -1, 0, 1, 0);   // GUIRO: a negative drum pitch keeps the token length (long vs short scrape)
    o_part("PIANO", pn, -1, 0, 0, 0); o_part("BASS", bs, -1, 0, 0, 0); o_part("TRES", tr, -1, 0, 0, 0); o_part("VIBES", vb, -1, 0, 0, 0);
    o_part("TRUMPET1", t1, t1b, 0, 0, 0); o_part("TRUMPET2", t2, t2b, 0, 0, 0); o_part("TROMBONE1", b1, b1b, 0, 0, 0); o_part("TROMBONE2", b2, b2b, 0, 0, 0);
    o_part("SOLO", so, -1, 0, 0, 0); o_part("SAX", sx, -1, 0, 0, 0); o_part("FLUTE", fl, -1, 0, 0, 0); o_part("STRINGS", st, st2, 0, 0, 0);
    int sus[] = {so, sx, fl, st, st2}; for (unsigned i = 0; i < sizeof sus / sizeof *sus; i++) g_o_sustain[g_o_nsus++] = sus[i];
    GUIRO = GS;
    // a club room, not a hall
    g_bus[0] = (Bus){.type = 1, .rev = {1.1, 0.6, 15, 0.8, 250, 7500, 0.15, 1.0, 0.25, 0.2}, .gain_db = -6, .duck_src = -1};
    g_master.eq[0] = (EqBand){2, 30, 0.7, 0}; g_master.eq[1] = (EqBand){5, 8000, 0, 1.5}; g_master.neq = 2;
    g_master.cthr = -16; g_master.cratio = 1.8; g_master.catt = 0.02; g_master.crel = 0.15; g_master.width_lo_hz = 120;
    g_tail_s = 4; g_dyn_depth = 0.8;
    // round 4: the low end measured -33 dB under 60 Hz; tape, a gentle top, mono lows already
    g_master.eq[2] = (EqBand){4, 80, 0, 5}; g_master.neq = 3; g_master.tape = 0.4; g_master.top_hz = 18000;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", o_map, 0);
    o_roll(n0, PN, 9, 184); o_roll(n0, TR, 14, 184); o_roll(n0, VB, 6, 184);   // the montuno's hands spread, the tres strummed
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) {
        Note a = g_note[i];
        if (a.tr == GUIRO && a.len >= 0.9) g_note[i].tr = GL;   // the long scrape on the beat, short ones between
        if (!strcmp(g_tr[a.tr].name, "tres")) note(TRES2, a.beat, a.len, a.pitch + 12, a.vel * 0.8);
        if (a.tr == BS) note(BSUB, a.beat, fmin(a.len, 1.0), a.pitch, a.vel);
    }
    { int si = sec_find("III La Latina: Rompe (percussion break)"); if (si >= 0) { double b0 = g_sec[si].bar0 * 4.0, b1 = b0 + 32;
        for (int i = n0; i < g_nnote; i++) if (g_note[i].beat >= b0 - 1e-6 && g_note[i].beat < b1 - 1e-6) { if (g_note[i].tr == CG) g_note[i].tr = CGS; else if (g_note[i].tr == BG) g_note[i].tr = BGS; } } }
    o_post(n0);
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
