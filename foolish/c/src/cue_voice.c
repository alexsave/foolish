// cue_voice.c - see cue_voice.h.

#include "cue_voice.h"

#include <stddef.h>

// [0, 1) from the state's own generator (xorshift32): the host seeds it once,
// and nothing here reads a clock.
static double draw(CueVoiceState *st) {
    uint32_t x = st->rng ? st->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    st->rng = x;
    return (double)(x >> 8) / (double)(1u << 24);
}

// A take that is one of `kind`'s, in `role`, at `level` (any level for -1).
static int is(const CueTakeRow *t, int kind, int role, int level) {
    return t->cue == kind && t->role == role && (level < 0 || t->level == level);
}

int cue_lead_us(const CueTakeRow *takes, int n_takes, int kind) {
    int lead = 0;
    for (int i = 0; takes && i < n_takes; i++)
        if (takes[i].cue == kind && takes[i].anchor_us > lead) lead = takes[i].anchor_us;
    return lead;
}

int cue_late_us(const CueTakeRow *takes, int n_takes, int kind) {
    int late = 0;
    for (int i = 0; takes && i < n_takes; i++) {
        if (takes[i].cue != kind || takes[i].role != CUE_ROLE_MAIN || takes[i].anchor_us >= 0) continue;
        if (late == 0 || -takes[i].anchor_us < late) late = -takes[i].anchor_us;
    }
    return late;
}

// The strokes a cue of n cards is gathered in: one per cards_per_level, 1 at
// least, as many as its preludes have; 0 for a cue with none, or no cards.
static int wanted_level(const CueTakeRow *takes, int n_takes, const CueMix *m, int kind, int n_cards) {
    if (m->cards_per_level_pm <= 0 || n_cards <= 0) return 0;
    int top = 0;
    for (int i = 0; i < n_takes; i++)
        if (is(&takes[i], kind, CUE_ROLE_PRELUDE, -1) && takes[i].level > top) top = takes[i].level;
    // round(n / per), half up, in thousandths
    int level = (int)((2LL * n_cards * 1000 + m->cards_per_level_pm) / (2LL * m->cards_per_level_pm));
    if (level < 1) level = 1;
    return level < top ? level : top;
}

// One of the takes `pick` admits, never the one this slot played last when
// there is another; -1 when it admits none.
typedef int (*Admit)(const CueTakeRow *t, int kind, int lead_us, int level);
static int variant(const CueTakeRow *takes, int n_takes, CueVoiceState *st, int slot,
                   Admit admit, int kind, int lead_us, int level) {
    int n = 0, others = 0;
    const int last = st->last[slot] - 1;
    for (int i = 0; i < n_takes; i++) {
        if (!admit(&takes[i], kind, lead_us, level)) continue;
        n++;
        if (i != last) others++;
    }
    if (n == 0) return -1;
    // A cue with one take plays it every time, and draws nothing for it: the
    // generator moves only for a choice, as the web's engine did
    // (tests/cue_voice_parity.h holds the two to the same draws).
    if (n == 1) {
        for (int i = 0; i < n_takes; i++) if (admit(&takes[i], kind, lead_us, level)) return i;
    }
    const int pool = others;
    int k = (int)(draw(st) * pool);
    if (k >= pool) k = pool - 1;
    for (int i = 0; i < n_takes; i++) {
        if (!admit(&takes[i], kind, lead_us, level) || i == last) continue;
        if (k-- == 0) {
            st->last[slot] = (int16_t)(i + 1);
            return i;
        }
    }
    return -1;
}
static int admit_main(const CueTakeRow *t, int kind, int lead_us, int level) {
    (void)lead_us; (void)level;
    return is(t, kind, CUE_ROLE_MAIN, -1);
}
static int admit_fit(const CueTakeRow *t, int kind, int lead_us, int level) {
    (void)level;
    return is(t, kind, CUE_ROLE_MAIN, -1) && t->anchor_us <= lead_us;
}
static int admit_tight(const CueTakeRow *t, int kind, int lead_us, int level) {
    (void)lead_us; (void)level;
    return is(t, kind, CUE_ROLE_TIGHT, -1);
}
static int admit_prelude(const CueTakeRow *t, int kind, int lead_us, int level) {
    (void)lead_us;
    return is(t, kind, CUE_ROLE_PRELUDE, level);
}
static int count(const CueTakeRow *takes, int n_takes, Admit admit, int kind, int lead_us, int level) {
    int n = 0;
    for (int i = 0; i < n_takes; i++) n += admit(&takes[i], kind, lead_us, level);
    return n;
}

// A play's level and speed: the cue's gain, the strike's own, each wandering
// as far as the mix lets it, and heavier per card.
static void level_and_rate(const CueMix *m, CueVoiceState *st, int n_cards, int gain_pm,
                           int32_t *out_gain, int32_t *out_rate) {
    const double rate = 1 + m->pitch_pm / 1000.0 * (2 * draw(st) - 1);
    const double wander = 1 + m->level_pm / 1000.0 * (2 * draw(st) - 1);
    double heavier = m->per_card_pm / 1000.0 * (n_cards > 1 ? n_cards - 1 : 0);
    if (heavier > 0.6) heavier = 0.6;
    const double g = m->gain_pm / 1000.0 * gain_pm / 1000.0 * wander * (1 + heavier);
    *out_gain = (int32_t)(g * 1000 + 0.5);
    *out_rate = (int32_t)(rate * 1000 + 0.5);
}

// When a take starts so that its anchor falls `lead_us` from now: the anchor
// early (late, for a negative one), at the play's speed, and never in the past.
static int32_t start_us(const CueTakeRow *t, int lead_us, int rate_pm, int backwards) {
    const double len_us = (double)t->n_samples * 1e6 / t->rate;
    const double anchor = (backwards ? len_us - t->anchor_us : t->anchor_us) * 1000.0 / rate_pm;
    const double s = lead_us - anchor;
    return s > 0 ? (int32_t)(s + 0.5) : 0;
}

int cue_strike(const CueTakeRow *takes, int n_takes, const CueMix *mix,
               CueVoiceState *st, int kind, int lead_us, int n_cards, int undoes,
               int gain_pm, CueStrike *out) {
    if (!out) return -1;
    *out = (CueStrike){ .take = -1, .prelude = -1 };
    const int backwards = kind == ANIM_CUE_REVERT;
    const int voiced = backwards ? undoes : kind;
    if (!takes || !mix || !st || voiced <= ANIM_CUE_NONE || voiced >= ANIM_N_CUES || voiced == ANIM_CUE_REVERT)
        return -1;
    if (lead_us < 0) lead_us = 0;
    const CueMix *m = &mix[voiced];

    // The cue's own takes whose anchor fits, else its tight ones, else any of
    // its own; a revert its own, backwards.
    Admit admit = admit_main;
    if (!backwards) {
        if (count(takes, n_takes, admit_fit, voiced, lead_us, 0) > 0) admit = admit_fit;
        else if (count(takes, n_takes, admit_tight, voiced, lead_us, 0) > 0) admit = admit_tight;
    }
    const int take = variant(takes, n_takes, st, voiced, admit, voiced, lead_us, 0);
    if (take < 0) return -1;
    out->take = take;
    out->backwards = backwards;
    level_and_rate(m, st, n_cards, gain_pm, &out->gain_pm, &out->rate_pm);
    out->start_us = start_us(&takes[take], lead_us, out->rate_pm, backwards);

    // The strokes that gather the table first, over whatever is sounding.
    const int level = backwards ? 0 : wanted_level(takes, n_takes, m, voiced, n_cards);
    if (level > 0) {
        const int p = variant(takes, n_takes, st, ANIM_N_CUES + voiced, admit_prelude, voiced, lead_us, level);
        if (p >= 0) {
            out->prelude = p;
            level_and_rate(m, st, n_cards, gain_pm, &out->prelude_gain_pm, &out->prelude_rate_pm);
            out->prelude_start_us = start_us(&takes[p], lead_us, out->prelude_rate_pm, 0);
        }
    }
    return 0;
}

int cue_resampled_len(int n, int rate_pm) {
    if (n <= 0 || rate_pm <= 0) return -1;
    return (int)((int64_t)(n - 1) * 1000 / rate_pm) + 1;
}

int cue_resample(const float *in, int n, int rate_pm, float *out, int cap) {
    const int len = cue_resampled_len(n, rate_pm);
    if (!in || !out || len < 0 || len > cap) return -1;
    for (int i = 0; i < len; i++) {
        const int64_t at = (int64_t)i * rate_pm;          // in thousandths of an input sample
        const int k = (int)(at / 1000);
        const float f = (float)(at % 1000) / 1000.0f;
        out[i] = k + 1 < n ? in[k] + (in[k + 1] - in[k]) * f : in[n - 1];
    }
    return len;
}
