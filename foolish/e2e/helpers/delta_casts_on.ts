// Imported FIRST by ui_animation_trace_delta.test.ts: the trace's server pushes
// delta casts (UI_ANIM_CASTS=delta) for every commit that may go as one. A module
// of its own because imports are evaluated in order, before any statement of the
// importing file runs.
if (process.env.UPDATE_UI_ANIM === '1') throw new Error('the goldens are recorded from ui_animation_trace.test.ts, with board casts');
process.env.UI_ANIM_CASTS = 'delta';
