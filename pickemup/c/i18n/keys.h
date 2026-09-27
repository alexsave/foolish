/* EVERY STRING THE GAME SAYS, by name, with the room each one has
 * (RULES_AND_KERNEL.md section 6).
 *
 * ONE LIST, AND IT IS THE ONLY PLACE A KEY IS DECLARED: the enum, the name a
 * test reports a hole by, each key's width limit and the label table the
 * generator reads all come out of PK_KEYS below, so they cannot disagree
 * (the uttt/c/i18n/keys.h shape).
 *
 * A LANGUAGE IS ONE strings_<code>.c in this directory, `[PK_K_...] = "..."`
 * for every key, designated so shared/tools/datagen reads it by name:
 *
 *   datagen --header keys.h --table PK_KEY_NAME --require-complete ...
 *   datagen --header strings_en.c --table PK_STRINGS_EN \
 *           --labels PK_STRINGS_EN.0=PK_KEY_NAME --require-complete ...
 *
 * English only for now; the table is compiled into the kernel because the
 * kernel composes the sentences (pk_say.c), as uttt's is.
 *
 * THE GAME'S NAME IS ONE KEY (GAME_NAME) and every line that says it uses
 * {game}, so renaming the game is one string (the name is a placeholder,
 * ORCHESTRATION.md O5). The trademark rules of pickemup/LEGAL.md bind every
 * line: the call-out is "Last card!", the catch is "Caught you!", and the
 * protected product's name never appears (tests/pk_say_test.c searches).
 *
 * THE LIMIT is in COLUMNS (pk_text_cols); 0 is no limit of its own (a line
 * that wraps, one only VoiceOver reads, or a template whose composed result
 * is what is checked - every caption against PK_CAPTION_MAX).
 *
 * PLACEHOLDERS: {who} {target} {next} {a} {b} a seat's name from the roster;
 * {card} a CARD_ phrase; {suits} a SUIT_ word; {rank} a number or RANK_
 * word; {n} a number; {state} SPOKEN_PLAYABLE or SPOKEN_NOT_PLAYABLE;
 * {game} GAME_NAME.
 *
 * No em dashes or en dashes in any language (shared/tools/release_strings
 * refuses them), and no line ends in a full stop (owner). */
#ifndef PK_I18N_KEYS_H
#define PK_I18N_KEYS_H

/* X(NAME, max columns, may be empty) */
#define PK_KEYS(X)                                                               \
    /* 6.1 things */                                                             \
    X(GAME_NAME,          20, 0)  /* "Pick 'Em Up"                            */ \
    X(SUIT_0,             12, 0)  /* "circles"                                */ \
    X(SUIT_1,             12, 0)                                                 \
    X(SUIT_2,             12, 0)                                                 \
    X(SUIT_3,             12, 0)                                                 \
    X(SUIT_ONE_0,         12, 0)  /* "circle"                                 */ \
    X(SUIT_ONE_1,         12, 0)                                                 \
    X(SUIT_ONE_2,         12, 0)                                                 \
    X(SUIT_ONE_3,         12, 0)                                                 \
    X(RANK_SKIP,          10, 0)  /* "skip", for {rank}                       */ \
    X(RANK_REVERSE,       10, 0)                                                 \
    X(RANK_PLUS2,         10, 0)                                                 \
    X(INDEX_PLUS4,         4, 0)  /* "+4", a Wild +4's corner index         */ \
    X(CARD_NUMBER,         0, 0)  /* "{rank} of {suits}"                      */ \
    X(CARD_SKIP,           0, 0)                                                 \
    X(CARD_REVERSE,        0, 0)                                                 \
    X(CARD_PLUS2,          0, 0)                                                 \
    X(CARD_WILD,          12, 0)                                                 \
    X(CARD_WILD4,         12, 0)                                                 \
    X(CALL_WORD,          14, 0)  /* "Last card!"                             */ \
    X(CAUGHT_WORD,        14, 0)  /* "Caught you!"                            */ \
    X(DIR_CW,             14, 0)                                                 \
    X(DIR_ACW,            14, 0)                                                 \
    X(DECK_LEFT,          10, 0)  /* "{n} left"                               */ \
    X(STAMP_LAST,          6, 0)  /* "LAST"                                   */ \
    X(STAMP_OUT,           6, 0)  /* "OUT", under the winner's badge          */ \
    X(STAMP_WRONG,        14, 0)  /* "Wrong call", under a wrong catcher       */ \
    X(SEAT_FALLBACK,      16, 0)  /* "Player {n}", a seat with no name        */ \
    /* 6.2 bubble captions: one line in the SENDER's language, composed by   \
     * pk_say_caption; never "you", every caption names the actor */           \
    X(CAP_JOIN,            4, 0)  /* ". " between two clauses                 */ \
    X(CAP_JOIN_BANG,       4, 0)  /* " " after a clause ending in "!"         */ \
    X(CAP_INVITE,          0, 0)                                                 \
    X(CAP_JOINED,          0, 0)                                                 \
    X(CAP_LEFT,            0, 0)                                                 \
    X(CAP_STARTED,         0, 0)                                                 \
    X(CAP_PLAYED,          0, 0)                                                 \
    X(CAP_PLAYED_WILD,     0, 0)                                                 \
    X(CAP_SKIPPED,         0, 0)                                                 \
    X(CAP_REVERSED,        0, 0)                                                 \
    X(CAP_REVERSE_2P,      0, 0)                                                 \
    X(CAP_PLUS2,           0, 0)                                                 \
    X(CAP_WILD4,           0, 0)                                                 \
    X(CAP_DREW_ONE,        0, 0)                                                 \
    X(CAP_DREW_N,          0, 0)                                                 \
    X(CAP_DREW_AND_PLAYED, 0, 0)                                                 \
    X(CAP_DREW_AND_PASSED, 0, 0)                                                 \
    X(CAP_PASSED,          0, 0)                                                 \
    X(CAP_RESHUFFLED,      0, 0)                                                 \
    X(CAP_SAID,            0, 0)                                                 \
    X(CAP_CAUGHT,          0, 0)                                                 \
    X(CAP_WRONG,           0, 0)                                                 \
    X(CAP_NEXT,            0, 0)                                                 \
    X(CAP_WON,             0, 0)                                                 \
    X(CAP_WON_WILD,        0, 0)                                                 \
    X(CAP_STUCK,           0, 0)                                                 \
    X(CAP_LONG,            0, 0)                                                 \
    /* 6.3 screen lines, drawn for one phone, so they may say "you" */         \
    X(HEAD_YOUR_TURN,     20, 0)                                                 \
    X(HEAD_WAITING,        0, 0)                                                 \
    X(HEAD_PICK_SUIT,     20, 0)                                                 \
    X(HEAD_STAGED,         0, 0)                                                 \
    X(HEAD_YOU_WIN,       20, 0)                                                 \
    X(HEAD_WINS,           0, 0)                                                 \
    X(SUB_MATCH,           0, 0)  /* "{suits}, or a {rank}"                   */ \
    X(SUB_MATCH_AN,        0, 0)  /* "{suits}, or an {rank}": SUB_MATCH before\
                                   * a rank word that pk_say reads as a vowel   \
                                   * sound ("8", or a word starting a e i o u);  \
                                   * a language with one article repeats SUB_MATCH */ \
    X(SUB_MATCH_WILD,      0, 0)                                                 \
    X(SUB_PLAYABLE_NONE,   0, 0)                                                 \
    X(SUB_ORDER,           0, 0)  /* "{a}, then {b}, then you"                */ \
    X(SUB_ORDER_1,         0, 0)  /* "{a}, then you"                          */ \
    X(SUB_ON_ONE,          0, 0)                                                 \
    X(SUB_SAID,            0, 0)                                                 \
    X(SUB_CAUGHT_YOU,      0, 0)                                                 \
    X(SUB_WRONG_YOU,       0, 0)                                                 \
    X(SUB_DRAWN_STAY,      0, 0)                                                 \
    X(SUB_STAGED_SKIP,     0, 0)                                                 \
    X(SUB_STAGED_PLUS,     0, 0)                                                 \
    X(SUB_STAGED_CALL,     0, 0)                                                 \
    X(TOAST_NO_MATCH,     28, 0)  /* a card dropped on the pile that does not play */ \
    X(BTN_DRAW,           14, 0)                                                 \
    X(BTN_PLAY,           14, 0)                                                 \
    X(BTN_PASS,           14, 0)                                                 \
    X(BTN_UNDO,           14, 0)  /* the Undo pill: a staged play comes back */ \
    X(BTN_SAY,            14, 0)                                                 \
    X(BTN_CAUGHT,         14, 0)                                                 \
    X(BTN_AGAIN,          14, 0)                                                 \
    X(BTN_RULES,          14, 0)                                                 \
    X(BTN_CANCEL,         14, 0)  /* the suit picker's x, spoken             */ \
    X(STRIP_DRAWS,         6, 0)  /* "×{n}", my staged draws, counted     */ \
    X(SEND_HINT,          10, 0)  /* under the arrow at Messages' Send (A15) */ \
    X(SPOKEN_FAN,          0, 0)                                                 \
    X(SPOKEN_CARD,         0, 0)                                                 \
    X(SPOKEN_PLAYABLE,     0, 0)                                                 \
    X(SPOKEN_NOT_PLAYABLE, 0, 0)                                                 \
    X(SPOKEN_DECK,         0, 0)                                                 \
    X(SPOKEN_STACK,        0, 0)                                                 \
    /* 6.4 the lobby */                                                        \
    X(LOBBY_TITLE,        20, 0)                                                 \
    X(LOBBY_WAITING,      24, 0)                                                 \
    X(LOBBY_ALONE,        24, 0)                                                 \
    X(LOBBY_FULL,         24, 0)                                                 \
    X(LOBBY_DEALER,        0, 0)                                                 \
    X(BTN_JOIN,           14, 0)                                                 \
    X(BTN_START,          14, 0)                                                 \
    X(BTN_LEAVE,          14, 0)                                                 \
    X(LOBBY_NAME_PROMPT,   0, 0)                                                 \
    X(LOBBY_ROW,           0, 0)  /* "{n}. {who}", a numbered roster row      */ \
    X(LOBBY_ROW_YOU,       0, 0)  /* "{n}. {who} (You)", this phone's row     */ \
    /* 6.5 errors and staleness */                                             \
    X(UNREADABLE,         24, 0)                                                 \
    X(UNREADABLE_WHY,      0, 0)                                                 \
    X(DAMAGED,             0, 0)                                                 \
    X(MOVED_ON,            0, 0)                                                 \
    X(LOST_RACE_PLAY,      0, 0)                                                 \
    X(LOST_RACE_CALL,      0, 0)                                                 \
    X(LOST_RACE_SAY,       0, 0)                                                 \
    /* 6.6 the rules page */                                                   \
    X(RULES_TITLE,         0, 0)                                                 \
    X(RULE_1,              0, 0)                                                 \
    X(RULE_2,              0, 0)                                                 \
    X(RULE_3,              0, 0)                                                 \
    X(RULE_4,              0, 0)                                                 \
    X(RULE_5,              0, 0)                                                 \
    X(RULE_6,              0, 0)                                                 \
    X(RULE_7,              0, 0)                                                 \
    X(RULE_8,              0, 0)

#define PK_KEY_ENUM(name, max, empty) PK_K_##name,
typedef enum { PK_KEYS(PK_KEY_ENUM) PK_K_COUNT } PkKey;
#undef PK_KEY_ENUM

enum { PK_RULES_N = PK_K_RULE_8 - PK_K_RULE_1 + 1 };

/* The key each slot answers to: the name a host and the generated modules
 * ask for. datagen reads this as dimension 0's labels, so the generated
 * tables are keyed by these and never by position. */
#define PK_KEY_LABEL(name, max, empty) [PK_K_##name] = #name,
static const char *const PK_KEY_NAME[PK_K_COUNT] = { PK_KEYS(PK_KEY_LABEL) };
#undef PK_KEY_LABEL

#endif
