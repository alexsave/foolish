// Tuning knobs for the bot strategies, with one precedence rule.
//
// A strategy's knobs (CD_BUDGET, OG_TRUMP_KEEP, ...) used to come from getenv
// alone, which made the deployed value a property of whoever set up the
// process: the server exported an env table per bot, the iOS bridge exported
// nothing, and the phone silently ran different bots than the website
// (docs/C_CORE_CONSOLIDATION.md §3.1). The knob VALUES now live in the C bot
// roster (bot_roster.h) and reach the strategies through here, so every host
// that names a bot gets that bot.
//
// Precedence, per §4.1 ("knob application becomes struct-driven, with env
// vars kept as research overrides"):
//
//     env var (if set and non-empty)   >   roster spec   >   C default
//
// Env winning is what keeps the arena sweeps, ablation switches and
// CD_W*/OG_W* overrides working exactly as before: a researcher exporting
// CD_BUDGET=max still gets max, whatever the roster says. Hosts that set no
// env — the phone, the iMessage extension, any future client — get the roster
// value instead of the C default, which is the bug this fixes.
//
// The roster spec is a compile-time constant ("CD_BUDGET=prod,CD_RACE=1"),
// installed for the duration of one bot_roster_choose call. Lookups are a
// linear scan over a handful of characters, run once per decision behind each
// strategy's flags-loaded latch, so the cost is noise next to a deliberation.

#ifndef CNITRO_BOT_KNOBS_H
#define CNITRO_BOT_KNOBS_H

// Install the roster knob spec: "KEY=VAL,KEY2=VAL2", or NULL/"" for none.
// The string is borrowed, not copied — callers pass a static const.
void bot_knobs_set(const char *spec);
void bot_knobs_clear(void);

// Look up `name`. Returns NUL-terminated value, or NULL when neither the env
// nor the spec defines it. The returned pointer is valid until the 4th
// subsequent bot_knob* call on this thread (values are copied into a small
// per-thread ring), which is ample for the read-once-into-a-static pattern
// every strategy uses. Values longer than 31 bytes are truncated; no knob is
// anywhere near that.
const char *bot_knob(const char *name);

// atoi(bot_knob(name)) with a default. Mirrors the old cd_env_int/og_env_int.
int bot_knob_int(const char *name, int def);
// True when set to something other than "0"/"". Mirrors the old cd_flag/og_flag.
int bot_knob_flag(const char *name);

#endif
