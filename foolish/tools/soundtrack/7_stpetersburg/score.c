// St. Petersburg - Russian ballet suite in Prokofiev's manner (round 3). Score text by Fable (score.txt); sampled orchestra.
// Anti-circus rules from the research: long heavy lows on beat 1, legato low strings + trombones on the bass, the theme in
// the middle-low register in unison/octaves (strings + horns + saxophone), no oom-pah, no piccolo lead.
#include "../engine/orch.h"
static int TPT, FLT;

void city_setup(void) {
    g_on_done = sections_out;
    // strings (sections), seating: violins left, violas centre-right, cellos right, basses far right
    int vln = o_smp("violins", OM "vln_sus.txt", 0.35, 1, -0.45, -30, 0.35, 6), vln2 = o_double(vln, "violins2", 0.15);
    o_hi(vln, "violins hi", 88);   // above D6: three solo violins with vibrato instead of the section stretched up
    int vlnS = o_smp("vln_spic", OM "vln_spic.txt", 0.12, 0, -0.45, -8, 0.3, 3), vlnT = o_smp("vln_trem", OM "vln_trem.txt", 0.3, 1, -0.45, -20, 0.35, -1);
    int vla = o_smp("violas", OM "vla_sus.txt", 0.35, 1, 0.15, -30, 0.35, -1);
    int vlaS = o_smp("vla_spic", OM "vla_spic.txt", 0.12, 0, 0.15, -8, 0.3, -1), vlaT = o_smp("vla_trem", OM "vla_trem.txt", 0.3, 1, 0.15, -20, 0.35, -2);
    int vc = o_smp("cellos", OM "vc_sus.txt", 0.35, 1, 0.4, -30, 0.3, 1), vc2 = o_double(vc, "cellos2", -0.1);
    int cb = o_smp("basses", OM "cb_sus.txt", 0.4, 1, 0.55, -35, 0.25, 5), cb2 = o_double(cb, "basses2", -0.1);
    // brass: every part doubled into a section
    int hn = o_smp("horns", OM "hn_sus.txt", 0.3, 1, -0.15, -35, 0.3, 0), hn2 = o_double(hn, "horns2", 0.2);
    int tp = o_smp("trumpets", OM "tpt_sus.txt", 0.2, 1, 0.1, -20, 0.25, -5), tp2 = o_double(tp, "trumpets2", 0.15);
    int tb = o_smp("trombones", OM "tbn_sus.txt", 0.25, 1, 0.3, -30, 0.25, 7), tb2 = o_double(tb, "trombones2", 0.15);
    int tu = o_smp("tuba", OM "tuba_sus.txt", 0.3, 1, 0.35, -40, 0.2, 4);
    // winds and colour
    int fl = o_smp("flute", OM "fl_vib.txt", 0.25, 1, -0.1, -20, 0.4, -1), ob = o_smp("oboe", OM "ob_vib.txt", 0.25, 1, 0.0, -20, 0.4, -2);
    int cl = o_smp("clarinet", OM "cl_sus.txt", 0.25, 1, 0.1, -20, 0.4, -2), bn = o_smp("bassoon", OM "bn_sus.txt", 0.25, 1, 0.2, -20, 0.35, -1);
    int sx = o_smp("sax", OM "tsax.txt", 0.25, 1, 0.05, -20, 0.35, 6);
    int hp = o_smp("harp", OM "harp.txt", 1.5, 0, -0.35, 0, 0.45, 4);
    int ce = o_smp("celesta", OM "glock2.txt", 1.2, 0, 0.25, 0, 0.45, 8), ce2 = o_smp("celesta2", OM "vibes.txt", 1.0, 0, 0.25, 0, 0.45, -6);
    int pn = o_smp("piano", OM "salamander.txt", 0.4, 0, 0.0, 0, 0.25, -3); g_tr[pn].sp[9] = 1;
    int ti = o_smp("timpani", OM "timpani.txt", 1.2, 0, 0.0, 0, 0.2, 1);
    int sn = o_smp("snare", OM "marchsnare.txt", 0.3, 0, -0.1, 0, 0.12, 1);
    int bd = o_smp("bassdrum", OM "bassdrum.txt", 1.0, 0, 0.05, 0, 0.15, 0);
    int cy = o_smp("cymbals", OM "clash.txt", 2.0, 0, 0.2, 0, 0.2, -5);
    o_part("VIOLINS", vln, vln2, 0, 0, 0); o_part("VIOLINS SPIC", vlnS, -1, 0, 0, 0); o_part("VIOLINS TREM", vlnT, -1, 0, 0, 0);
    o_part("VIOLAS", vla, -1, 0, 0, 0); o_part("VIOLAS SPIC", vlaS, -1, 0, 0, 0); o_part("VIOLAS TREM", vlaT, -1, 0, 0, 0);
    o_part("CELLOS", vc, vc2, 0, 0, 0); o_part("BASSES", cb, cb2, 0, 0, 0);
    o_part("HORNS", hn, hn2, 0, 0, 0); o_part("TRUMPETS", tp, tp2, 0, 0, 0); o_part("TROMBONES", tb, tb2, 0, 0, 0); o_part("TUBA", tu, -1, 0, 0, 0);
    o_part("FLUTE", fl, -1, 0, 0, 0); o_part("OBOE", ob, -1, 0, 0, 0); o_part("CLARINET", cl, -1, 0, 0, 0); o_part("BASSOON", bn, -1, 0, 0, 0);
    o_part("SAX", sx, -1, 0, 0, 0); o_part("HARP", hp, -1, 0, 0, 0); o_part("CELESTA", ce, ce2, 0, 0, 0); o_part("PIANO", pn, -1, 0, 0, 0);
    o_part("TIMPANI", ti, -1, 0, 0, 0); o_part("SNARE", sn, -1, 0, 1, 0); o_part("BASSDRUM", bd, -1, 0, 1, 0); o_part("CYMBALS", cy, -1, 0, 1, -60);
    int sus[] = {vln, vln2, vla, vc, vc2, cb, cb2, hn, hn2, tp, tp2, tb, tb2, tu, fl, ob, cl, bn, sx}; for (unsigned i = 0; i < sizeof sus / sizeof *sus; i++) g_o_sustain[g_o_nsus++] = sus[i];
    eq(vln, 3, 250, 1, -2); eq(vln, 5, 4000, 0, 2); eq(hn, 3, 300, 1, -1.5); eq(tb, 3, 3000, 1, -2);
    TPT = tp; FLT = fl;
    o_hall(2.4, 18); g_dyn_depth = 0.75;   // the score's dynamics, compressed for a game bed
}
double city_loop(void) { int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", o_map, 0);
    // the trumpet samples stop at C6: notes above D6 were the samples stretched up (synthetic); the trumpets take them an octave
    // down and the flute keeps the written pitch on top
    for (int i = n0, n1 = g_nnote; i < n1; i++) if (g_note[i].tr == TPT && g_note[i].pitch > 86) { Note a = g_note[i]; g_note[i].pitch -= 12; note(FLT, a.beat, a.len, a.pitch, a.vel * 0.9); }
    o_post(n0); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
