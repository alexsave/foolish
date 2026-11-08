// models.h - the card sounds Alex approved by ear, ported from their Python.
//
// One function a model, each a line-by-line port of the prototype named beside
// it (that file and its folder's notes hold the measurements, the reasoning and
// Alex's feedback; this holds only the arithmetic). Each returns one take at
// 48 kHz, at the level its Python writes, with its reference point (the moment
// the game lines it up by) where the Python puts it; *_REF_S says where.
//
// How every one of them was checked against its Python and the recordings:
// docs/AUDIO.md, "The card sounds".
#ifndef SOUNDGEN_MODELS_H
#define SOUNDGEN_MODELS_H

#include "dsp.h"

// deal/an/thumb2.py thumb9: the dealer's thumb sliding the top card over.
Sig m_thumb(Rng *r);
// deal/an/grab2.py: the other hand's fingers taking it. Lined up by its peak.
Sig m_grab(Rng *r);
// deal/an/land_syn.py land229 2: the dealt card on the cloth, the table ringing at 229 Hz.
Sig m_land(Rng *r);
// throwin/an/throw_syn.py throw9 7: a card thrown onto the cloth.
Sig m_throw(Rng *r);
// cover/an/cov_syn.py cover3 7: a card thrown onto a card.
Sig m_cover(Rng *r);
// throwin/an/tgrab_syn.py tgrab2 9: two fingers taking a card in the hand. Its contact at TGRAB_REF_S.
#define TGRAB_REF_S 0.2
Sig m_tgrab(Rng *r);
// flip/an/flip_syn.py flipE 3: the trump pushed off and turned over. Its snap at FLIP_REF_S.
#define FLIP_REF_S 0.35
Sig m_flip(Rng *r);
// flip/an/deck_syn.py deck229c 5: the stock set down on the trump. Its thud at DECK_REF_S.
#define DECK_REF_S 0.2
Sig m_deck(Rng *r);
// pass/an/pass_syn.py pass7 4: the "shfff" of a card slid across the cloth. Its onset at PASS_REF_S.
#define PASS_REF_S 0.1
Sig m_pass(Rng *r);

// discard/an/disc_syn.py (SHFF = 1): the parts of a sweep and a pickup.
// A stroke of the hand gathering the table, `dur` s long (0 draws one from the
// recordings), its onset at STROKE_PRE_S.
#define STROKE_PRE_S 0.06
Sig m_stroke(Rng *r, double dur);
// One of the recorded strokes' lengths, drawn (disc_game.py's rng.choice(D.SCRD)).
double m_stroke_dur(Rng *r);
// The pile lifted and turned (discard), its peak at EVENT_PRE_S.
#define EVENT_PRE_S 0.08
Sig m_lift(Rng *r);
// The pile set on the discard pile.
Sig m_place(Rng *r);
// The pickup's one soft lift.
Sig m_plift(Rng *r);
// The picked-up cards slid into the fanned hand, its onset at STROKE_PRE_S.
Sig m_insert(Rng *r);

#endif
