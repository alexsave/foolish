/* EVERY STRING THE GAME SAYS, by name, with the room each one has
 * (DECISIONS.md T8; pickemup/c/i18n/keys.h's shape).
 *
 * ONE LIST, AND IT IS THE ONLY PLACE A KEY IS DECLARED: the enum, the name a
 * test reports a hole by, each key's width limit and the label table the
 * generator reads all come out of TB_KEYS below, so they cannot disagree.
 *
 * A LANGUAGE IS ONE strings_<code>.c in this directory, `[TB_K_...] = "..."`
 * for every key, designated so shared/tools/datagen reads it by name:
 *
 *   datagen --header keys.h --table TB_KEY_NAME --require-complete ...
 *   datagen --header strings_en.c --table TB_STRINGS_EN \
 *           --labels TB_STRINGS_EN.0=TB_KEY_NAME --require-complete ...
 *
 * English only for the proof of concept; the table is compiled into the
 * kernel because the kernel composes the sentences (tb_say.c).
 *
 * THE GAME'S NAME IS ONE KEY (GAME_NAME) and every line that says it uses
 * {game}, the five-alike category's name and its shout included, so renaming
 * the game is one string (T1). The branded game's name and its card's words
 * never appear (T4, tests/tb_say_test.c searches).
 *
 * THE LIMIT is in COLUMNS (tb_text_cols); 0 is no limit of its own (a line
 * that wraps, one only VoiceOver reads, or a template whose composed result
 * is what is checked: every caption against TB_CAPTION_MAX).
 *
 * PLACEHOLDERS: {who} {next} {a} {b} a seat's name from the roster, {a} also
 * a list of them; {n} a number; {cat} a CAT_ word; {phrase} a PHRASE_ word;
 * {dice} kept dice as numbers; {count} a NUM_ word; {face} a NUM_ word;
 * {total} a number; {game} GAME_NAME.
 *
 * No em dashes or en dashes in any language, and no line ends in a full
 * stop. */
#ifndef TB_I18N_KEYS_H
#define TB_I18N_KEYS_H

/* X(NAME, max columns, may be empty) */
#define TB_KEYS(X)                                                               \
    /* things */                                                                 \
    X(GAME_NAME,          20, 0)  /* "Tallybones"                             */ \
    X(CAT_0,              14, 0)  /* "Ones": the card's names, T4 order       */ \
    X(CAT_1,              14, 0)                                                 \
    X(CAT_2,              14, 0)                                                 \
    X(CAT_3,              14, 0)                                                 \
    X(CAT_4,              14, 0)                                                 \
    X(CAT_5,              14, 0)                                                 \
    X(CAT_6,              14, 0)                                                 \
    X(CAT_7,              14, 0)                                                 \
    X(CAT_8,              14, 0)                                                 \
    X(CAT_9,              14, 0)                                                 \
    X(CAT_10,             14, 0)                                                 \
    X(CAT_11,             14, 0)  /* "{game}": the five-alike category        */ \
    X(CAT_12,             14, 0)                                                 \
    X(PHRASE_6,            0, 0)  /* "three alike", for {phrase}              */ \
    X(PHRASE_7,            0, 0)                                                 \
    X(PHRASE_8,            0, 0)  /* "a full house"                           */ \
    X(PHRASE_9,            0, 0)                                                 \
    X(PHRASE_10,           0, 0)                                                 \
    X(NUM_1,               8, 0)  /* "one", for {count} and {face}            */ \
    X(NUM_2,               8, 0)                                                 \
    X(NUM_3,               8, 0)                                                 \
    X(NUM_4,               8, 0)                                                 \
    X(NUM_5,               8, 0)                                                 \
    X(NUM_6,               8, 0)                                                 \
    X(LIST_SEP,            4, 0)  /* ", " between dice and between names      */ \
    X(SEAT_FALLBACK,      16, 0)  /* "Player {n}", a seat with no name        */ \
    X(ROW_UPPER,          14, 0)  /* the numbers half's subtotal row          */ \
    X(ROW_BONUS,          14, 0)                                                 \
    X(ROW_TOTAL,          14, 0)                                                 \
    X(SHOUT,              16, 0)  /* "{game}!", five alike on the table       */ \
    /* bubble captions: one line in the SENDER's language, never "you"      */ \
    X(CAP_JOIN,            4, 0)  /* ". " between two clauses                 */ \
    X(CAP_JOIN_BANG,       4, 0)  /* " " after a clause ending in "!"         */ \
    X(CAP_INVITE,          0, 0)                                                 \
    X(CAP_JOINED,          0, 0)                                                 \
    X(CAP_LEFT,            0, 0)                                                 \
    X(CAP_STARTED,         0, 0)                                                 \
    X(CAP_KEEP,            0, 0)  /* "{who} keeps {dice} and rerolls {count}" */ \
    X(CAP_KEEP_NONE,       0, 0)                                                 \
    X(CAP_SCORED_UPPER,    0, 0)  /* "{who} scored {n} in {cat}"              */ \
    X(CAP_SCORED_COMBO,    0, 0)  /* "{who} rolled {phrase}, {n} points"      */ \
    X(CAP_SCORED_SHOUT,    0, 0)  /* "{who} rolled {game}! {n} points"        */ \
    X(CAP_SCORED_ANY,      0, 0)                                                 \
    X(CAP_ZERO,            0, 0)  /* "{who} took a zero on {cat}"             */ \
    X(CAP_BONUS,           0, 0)                                                 \
    X(CAP_QUIT,            0, 0)  /* a leave in a live game                   */ \
    X(CAP_NEXT,            0, 0)  /* "{next} to roll"                         */ \
    X(CAP_WON,             0, 0)  /* "{who} wins with {n}"                    */ \
    X(CAP_TIE,             0, 0)  /* "{a} and {b} tie at {n}"                 */ \
    /* screen lines, drawn for one phone, so they may say "you"             */ \
    X(HEAD_YOUR_ROLL,     24, 0)                                                 \
    X(HEAD_WAITING,        0, 0)                                                 \
    X(HEAD_STAGED_KEEP,   24, 0)                                                 \
    X(HEAD_STAGED_SCORE,  24, 0)                                                 \
    X(HEAD_STAGED_LEAVE,  24, 0)                                                 \
    X(HEAD_YOU_WIN,       24, 0)                                                 \
    X(HEAD_WINS,           0, 0)                                                 \
    X(HEAD_TIE,            0, 0)                                                 \
    X(HEAD_LEFT,          24, 0)                                                 \
    X(SUB_ROLLS_2,         0, 0)                                                 \
    X(SUB_ROLLS_1,         0, 0)                                                 \
    X(SUB_ROLLS_0,         0, 0)                                                 \
    X(SUB_STAGED_KEEP,     0, 0)                                                 \
    X(SUB_STAGED_SCORE,    0, 0)  /* "{cat} for {n}"                          */ \
    X(SUB_THEIR_ROLL,      0, 0)  /* "{who} is on roll {n} of 3"              */ \
    X(SUB_OVER,            0, 0)                                                 \
    X(BTN_REROLL,         14, 0)                                                 \
    X(BTN_SCORE,          14, 0)                                                 \
    X(BTN_JOIN,           14, 0)                                                 \
    X(BTN_START,          14, 0)                                                 \
    X(BTN_LEAVE,          14, 0)                                                 \
    X(BTN_CANCEL,         14, 0)                                                 \
    X(BTN_RULES,          14, 0)                                                 \
    X(BTN_AGAIN,          14, 0)                                                 \
    X(SPOKEN_DIE,          0, 0)  /* "Die {n}, {face}"                        */ \
    X(SPOKEN_DIE_KEPT,     0, 0)                                                 \
    X(SPOKEN_DIE_UNKNOWN,  0, 0)                                                 \
    /* the lobby */                                                            \
    X(LOBBY_WAITING,      24, 0)                                                 \
    X(LOBBY_ALONE,        24, 0)                                                 \
    X(LOBBY_FULL,         24, 0)                                                 \
    X(LOBBY_NAME_PROMPT,   0, 0)                                                 \
    X(LOBBY_ROW,           0, 0)  /* "{n}. {who}"                             */ \
    X(LOBBY_ROW_YOU,       0, 0)  /* "{n}. {who} (You)"                       */ \
    X(RANK_ROW,            0, 0)  /* "{n}. {who}, {total}"                    */ \
    /* errors */                                                               \
    X(UNREADABLE,         24, 0)                                                 \
    X(UNREADABLE_WHY,      0, 0)                                                 \
    X(DAMAGED,             0, 0)                                                 \
    X(STAGED_OWN,          0, 0)                                                 \
    /* the rules page */                                                       \
    X(RULES_TITLE,         0, 0)                                                 \
    X(RULE_1,              0, 0)                                                 \
    X(RULE_2,              0, 0)                                                 \
    X(RULE_3,              0, 0)                                                 \
    X(RULE_4,              0, 0)                                                 \
    X(RULE_5,              0, 0)                                                 \
    X(RULE_6,              0, 0)

#define TB_KEY_ENUM(name, max, empty) TB_K_##name,
typedef enum { TB_KEYS(TB_KEY_ENUM) TB_K_COUNT } TbKey;
#undef TB_KEY_ENUM

enum { TB_RULES_N = TB_K_RULE_6 - TB_K_RULE_1 + 1 };

/* The key each slot answers to: the name a host and the generated modules
 * ask for, datagen's dimension-0 labels. */
#define TB_KEY_LABEL(name, max, empty) [TB_K_##name] = #name,
static const char *const TB_KEY_NAME[TB_K_COUNT] = { TB_KEYS(TB_KEY_LABEL) };
#undef TB_KEY_LABEL

#endif
