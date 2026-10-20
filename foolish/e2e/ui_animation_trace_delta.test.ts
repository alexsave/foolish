// The live game's moves through the real page, every move's push a DELTA cast
// (c/src/evwire.h EVW_CAST_DELTA): the page's kernel rebuilds each one on the
// board it kept, and every frame it draws must be the frame the board cast drew.
// Same cases, same goldens (e2e/fixtures/ui_anim/*.txt) as
// ui_animation_trace.test.ts; never re-record them from here.
import './helpers/delta_casts_on.ts';
import './ui_animation_trace.test.ts';
