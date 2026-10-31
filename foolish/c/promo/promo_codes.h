// The promo codes: the short paths printed on the promo playing cards, each a
// QR code (foolish.cards/br). Opening one drops the visitor straight into a game
// against a bot, played in the tab (c/src/local_table.h): no server, no sign-up,
// no account. The code names where the cards were handed out and the language
// the game opens in.
//
// THIS FILE IS SOURCE FOR A GENERATOR, like c/cardtext: tools/datagen reads the
// table below at build time and writes sdk/ts/gen/promo_codes.ts
// (tools/structgen/gen.sh), which the web's route reads (src/common/promo.ts).
// No kernel object includes it.
//
// THE RULES A ROW KEEPS (e2e/validation/promo_codes_validation.test.ts holds
// every row to them):
//   - `code` is lowercase ASCII letters and is matched in any case (/BR, /Br);
//   - `code` has a letter outside 0-9a-f, so it can never be a game id (those are
//     exactly six of "0123456789abcdef", server/src/store/game_id.h) and is
//     shorter than any replay code;
//   - `lang` is a language c/i18n declares, and may be left out: a code with
//     none opens in PROMO_LANG_DEFAULT (English);
//   - `src` names the place, once: what a request log or a later count reads.
#ifndef FOOLISH_PROMO_CODES_H
#define FOOLISH_PROMO_CODES_H

typedef struct { const char *code; const char *src; const char *lang; } PromoCode;

// The language of a code whose row names none. A one-cell table so the
// generator carries it to the web: the default has one owner, here.
static const char *const PROMO_LANG_DEFAULT[] = { "en" };

static const PromoCode PROMO_CODES[] = {
    { .code = "br", .src = "brighton",          .lang = "ru" },   // Brighton Beach
    { .code = "ws", .src = "washington-square" },                 // Washington Square Park: the default
};

#endif
