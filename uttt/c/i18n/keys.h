/* EVERY STRING THE GAME SAYS, by name, with the room each one has.
 *
 * ONE LIST, AND IT IS THE ONLY PLACE A KEY IS DECLARED: the enum, the name a
 * test reports a hole by, and each key's width limit all come out of UT_KEYS
 * below, so the three cannot disagree. A language is one strings_<code>.c in
 * this directory, `[UT_K_...] = "..."` for every key; the registry of which
 * languages exist is shared/c/i18n/languages.h (the same list the sister
 * product carries), and src/uttt_lang.c joins the two.
 *
 * UNLIKE THE SISTER PRODUCT'S TABLE, THIS ONE IS COMPILED INTO THE KERNEL.
 * There the strings are data for a generator and the hosts look them up; here
 * the kernel composes the sentences itself (uttt_say.c: which words go with
 * which position is a question about the game), so it has to hold the words.
 * The iOS library carries every language, about 30 KB; the replay page's wasm
 * is built with UTTT_LANG_ENGLISH_ONLY and carries one (uttt/c/Makefile).
 *
 * A HOLE IS A TEST FAILURE, NOT A BLANK. C fills an undesignated slot with
 * NULL, uttt_text falls back to English for one, and tests/uttt_lang_test.c
 * refuses the build before that fallback can ship: every language fills every
 * key, keeps English's {placeholders}, and fits each key's limit.
 *
 * THE LIMIT is in COLUMNS (uttt_text_cols: a CJK or Hangul character is two,
 * a combining mark none, anything else one), because what runs out is width
 * on a phone and a Japanese character is as wide as two Latin ones. 0 is no
 * limit of its own: a sentence that wraps (a rule, a lobby line), one only
 * VoiceOver reads, or a template whose composed result is what gets checked
 * (every caption against UTTT_CAPTION_MAX).
 *
 * PLACEHOLDERS: {who} the sender ("X", "O" or Messages' "$<uuid>" name
 * token), {mark} a side ("X" or "O"),
 * {moves} a MOVES_ phrase, {n} the number of moves, {board} {cell} PLACE_
 * phrases, {state} "X", "O" or CELL_EMPTY. A template may use {n} in place of
 * {moves} where its grammar needs a case the MOVES_ forms are not in (German
 * "in 25 Zügen" is dative; the place line's "25 Züge" is not).
 *
 * THE MARKS ARE NEVER TRANSLATED. X and O are what the board draws.
 *
 * WHERE A DRAWN MARK SITS IN A LINE OF WORDS, the words are split around it
 * by POSITION ON THE SCREEN, not by reading order: _PRE is set left of the
 * mark and _POST right of it, and the watch line's and the bubble's words are
 * always right of it. So a right-to-left language puts the words it reads
 * first in _POST, with its gap to the mark at its logical end ("התור של "),
 * and no renderer has to know which way a language runs. Either half may be
 * "", but not both.
 *
 * No em dashes in any language (shared/tools/release_strings refuses them in
 * a Release build), and a caption or a label ends without a full stop (owner). */
#ifndef UTTT_I18N_KEYS_H
#define UTTT_I18N_KEYS_H

/* X(NAME, max columns, may be empty) */
#define UT_KEYS(X)                                                                  \
    /* THE CAPTION, one transcript line (UTTT_CAPTION_MAX), in the SENDER's     \
     * language: it is baked into the message the sender stages, so both       \
     * phones read the sender's words (uttt_say.h) */                           \
    X(CAP_NEW_GAME,       0, 0)  /* "New game?"                              */ \
    X(CAP_INVITE,         0, 0)  /* "{who} wants a game. Tap to take it"     */ \
    X(CAP_TO_PLAY,        0, 0)  /* "{mark} to play"                         */ \
    X(CAP_WON,            0, 0)  /* "{who} won in {moves}"                   */ \
    X(CAP_DRAWN,          0, 0)  /* "Drawn in {moves}"                       */ \
    /* how many moves, by the language's plural category (uttt_plural); a     \
     * language without a category repeats its OTHER form there */             \
    X(MOVES_ONE,          0, 0)  /* "{n} move"                               */ \
    X(MOVES_FEW,          0, 0)                                                 \
    X(MOVES_MANY,         0, 0)                                                 \
    X(MOVES_OTHER,        0, 0)  /* "{n} moves"                              */ \
    /* how a won line is said in a caption, numbered as uttt_line_mask */      \
    /* THE END'S SUBLINE, under the verdict: the won line by its shape, or   \
     * a draw's none, numbered as the LINE_ keys and worded to agree */        \
    X(END_ROW_TOP,       24, 0)  /* "Top row"                                */ \
    X(END_ROW_MIDDLE,    24, 0)                                                 \
    X(END_ROW_BOTTOM,    24, 0)                                                 \
    X(END_COL_LEFT,      24, 0)  /* "Left column"                            */ \
    X(END_COL_MIDDLE,    24, 0)                                                 \
    X(END_COL_RIGHT,     24, 0)                                                 \
    X(END_DIAGONAL,      24, 0)  /* "Diagonal"                               */ \
    X(END_DRAW,          24, 0)  /* "Nine blocks, no line"                   */ \
    /* THE BUBBLE'S WORDS, baked into its image beside the board: about 95   \
     * points of bold 18, the winner's mark drawn before BUBBLE_WINS */         \
    X(BUBBLE_WINS,        7, 0)  /* "wins"                                   */ \
    X(BUBBLE_DRAW,       10, 0)  /* "A draw"                                 */ \
    /* THE PLAY SURFACE'S HEADLINE, drawn for one phone */                     \
    X(HEAD_DRAWN,        16, 0)  /* "Drawn", also the spectator's line       */ \
    X(HEAD_YOU_WIN,      16, 0)  /* "You win"                                */ \
    X(HEAD_YOUR_MOVE,    16, 0)  /* "Your move"                              */ \
    X(HEAD_WAITING_PRE,  16, 1)  /* "Waiting on " <O>                        */ \
    X(HEAD_WAITING_POST, 16, 1)  /*                                          */ \
    X(HEAD_WINS_PRE,     16, 1)  /*                                          */ \
    X(HEAD_WINS_POST,    16, 1)  /* <X> " wins"                              */ \
    X(SPOKEN_WAITING,     0, 0)  /* VoiceOver: "Waiting on {mark}"           */ \
    X(SPOKEN_WINS,        0, 0)  /* VoiceOver: "{mark} wins"                 */ \
    /* the spectator's one line, after a drawn mark */                         \
    X(WATCH_LABEL,       12, 0)  /* "watching"                               */ \
    X(WATCH_TO_PLAY,     16, 0)  /* <O> " to play"                           */ \
    X(WATCH_TOOK,        16, 0)  /* <X> " took it"                           */ \
    X(SPOKEN_WATCH_TOOK,  0, 0)  /* VoiceOver: "{mark} took it"              */ \
    /* the lobby, and a bubble that cannot be read */                          \
    X(LOBBY_WAITING,     16, 0)  /* "Waiting"                                */ \
    X(LOBBY_NOBODY,       0, 0)  /* "Nobody has taken it yet"; wraps         */ \
    X(UNREADABLE,        24, 0)  /* "Can't read that"                        */ \
    X(UNREADABLE_WHY,     0, 0)  /* "That board came from a newer ..."       */ \
    /* the "you are" indicator: two small lines over the drawn mark */         \
    X(YOU_ARE_1,         10, 0)  /* "you"                                    */ \
    X(YOU_ARE_2,         10, 0)  /* "are"                                    */ \
    X(SPOKEN_YOU_ARE,     0, 0)  /* VoiceOver: "You are {mark}"              */ \
    /* the doors: 14-point bold across a door the width of half a sheet */     \
    X(DOOR_AGAIN,        16, 0)  /* "Again"                                  */ \
    X(DOOR_RULES,         0, 0)  /* VoiceOver: "Rulebook" (the door is ink)  */ \
    X(SEND_HINT,         10, 0)  /* "Send", under the arrow at Messages' Send */\
    X(DOOR_SEND,         20, 0)  /* "Send a board"                           */ \
    X(DOOR_COPY,         16, 0)  /* "Copy code"                              */ \
    X(DOOR_COPIED,       16, 0)  /* "Copied"                                 */ \
    /* VoiceOver on a square (uttt_say_cell) */                                \
    X(CELL,               0, 0)  /* "{board} board, {cell} square, {state}"  */ \
    X(CELL_EMPTY,         0, 0)  /* "empty"                                  */ \
    X(PLACE_TOP_LEFT,     0, 0)  /* the nine blocks, and "anywhere"          */ \
    X(PLACE_TOP,          0, 0)                                                 \
    X(PLACE_TOP_RIGHT,    0, 0)                                                 \
    X(PLACE_LEFT,         0, 0)                                                 \
    X(PLACE_CENTRE,       0, 0)                                                 \
    X(PLACE_RIGHT,        0, 0)                                                 \
    X(PLACE_BOTTOM_LEFT,  0, 0)                                                 \
    X(PLACE_BOTTOM,       0, 0)                                                 \
    X(PLACE_BOTTOM_RIGHT, 0, 0)                                                 \
    X(PLACE_ANYWHERE,     0, 0)                                                 \
    /* THE RULES SHEET: a title and eight lines, docs/RULES.html. The two    \
     * yellow phrases are drawn round (the outline) and behind (the tint) in \
     * rules 6 and 7, so each must appear in its rule word for word */        \
    X(RULES_TITLE,       30, 0)                                                 \
    X(RULE_1,             0, 0)                                                 \
    X(RULE_2,             0, 0)                                                 \
    X(RULE_3,             0, 0)                                                 \
    X(RULE_4,             0, 0)                                                 \
    X(RULE_5,             0, 0)                                                 \
    X(RULE_6,             0, 0)                                                 \
    X(RULE_7,             0, 0)                                                 \
    X(RULE_8,             0, 0)                                                 \
    X(RULES_OUTLINE,      0, 0)  /* "yellow outline", in RULE_6              */ \
    X(RULES_TINT,         0, 0)  /* "yellow tinted area", in RULE_7          */

#define UT_KEY_ENUM(name, max, empty) UT_K_##name,
enum { UT_KEYS(UT_KEY_ENUM) UT_K_COUNT };
#undef UT_KEY_ENUM

enum { UT_RULES_N = UT_K_RULE_8 - UT_K_RULE_1 + 1 };

#endif
