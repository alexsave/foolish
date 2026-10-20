// The live game's moves through the real page, every move's push a DELTA cast
// (c/src/evwire.h EVW_CAST_DELTA): the page's kernel rebuilds each one on the
// board it kept, and every frame it draws must be the frame the board cast drew.
// Same cases, same goldens (e2e/fixtures/ui_anim/*.txt) as
// ui_animation_trace.test.ts; never re-record them from here.
process.env.UI_ANIM_CASTS = 'delta';
if (process.env.UPDATE_UI_ANIM === '1') throw new Error('the goldens are recorded from ui_animation_trace.test.ts, with board casts');
await import('./ui_animation_trace.test.ts');
