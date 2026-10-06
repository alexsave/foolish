/* EVERY STRING THE GAME SAYS, by name, with the room each one has.
 *
 * PICK 'EM UP'S SHAPE (pickemup/c/i18n/keys.h): ONE LIST, AND IT IS THE ONLY
 * PLACE A KEY IS DECLARED. The enum, the name a test reports a hole by, each
 * key's width limit and the label table shared/tools/datagen reads all come
 * out of CN_KEYS below, so they cannot disagree.
 *
 * A LANGUAGE IS ONE strings_<code>.c in this directory, `[CN_K_...] = "..."`
 * for every key. English only for the proof of concept; the table is
 * compiled into the kernel because the kernel composes the sentences
 * (cn_say.c).
 *
 * THE GAME'S NAME IS ONE KEY (GAME_NAME) and every line that says it uses
 * {game}, so renaming the game is one string (DECISIONS O2).
 *
 * THE LIMIT is in COLUMNS (cn_text_cols); 0 is no limit of its own (a line
 * that wraps, or a template whose composed result is what is checked).
 *
 * PLACEHOLDERS: {who} {loser} a seat's name from the roster; {bid} a
 * BID_ONE / BID_MANY phrase (sentence-initial where the template starts
 * with it); {qty} a NUM_ word or a number; {face} a face digit; {n} a
 * number; {game} GAME_NAME; {loss} CAP_LOSES composed.
 *
 * No em dashes or en dashes, and no line ends in a full stop
 * (tests/cn_say_test.c refuses both). */
#ifndef CN_I18N_KEYS_H
#define CN_I18N_KEYS_H

/* X(NAME, max columns, may be empty) */
#define CN_KEYS(X)                                                               \
    X(GAME_NAME,          20, 0)  /* "Chui Niu"                               */ \
    /* quantities as words, and the same at the start of a sentence */         \
    X(NUM_1,              10, 0)                                                 \
    X(NUM_2,              10, 0)                                                 \
    X(NUM_3,              10, 0)                                                 \
    X(NUM_4,              10, 0)                                                 \
    X(NUM_5,              10, 0)                                                 \
    X(NUM_6,              10, 0)                                                 \
    X(NUM_7,              10, 0)                                                 \
    X(NUM_8,              10, 0)                                                 \
    X(NUM_9,              10, 0)                                                 \
    X(NUM_10,             10, 0)                                                 \
    X(NUM_11,             10, 0)                                                 \
    X(NUM_12,             10, 0)                                                 \
    X(NUMCAP_1,           10, 0)                                                 \
    X(NUMCAP_2,           10, 0)                                                 \
    X(NUMCAP_3,           10, 0)                                                 \
    X(NUMCAP_4,           10, 0)                                                 \
    X(NUMCAP_5,           10, 0)                                                 \
    X(NUMCAP_6,           10, 0)                                                 \
    X(NUMCAP_7,           10, 0)                                                 \
    X(NUMCAP_8,           10, 0)                                                 \
    X(NUMCAP_9,           10, 0)                                                 \
    X(NUMCAP_10,          10, 0)                                                 \
    X(NUMCAP_11,          10, 0)                                                 \
    X(NUMCAP_12,          10, 0)                                                 \
    X(BID_ONE,             0, 0)  /* "one 3"                                  */ \
    X(BID_MANY,            0, 0)  /* "four 3s"                                */ \
    X(DICE_ONE,           10, 0)  /* "1 die", a seat's count                  */ \
    X(DICE_MANY,          10, 0)  /* "4 dice"                                 */ \
    X(SEAT_FALLBACK,      16, 0)  /* "Player 3"                               */ \
    /* bubble captions: one line shown on every phone, so never "you"       */ \
    X(CAP_START,           0, 0)  /* "Dice rolled. Alex bids first"           */ \
    X(CAP_BID,             0, 0)  /* "Alex bid four 3s"                       */ \
    X(CAP_CALL,            0, 0)  /* "Bo calls four 3s": no outcome (K8)      */ \
    /* the outcome of a call, a screen line once it is sent (K8)            */ \
    X(CAP_CALL_TRUE,       0, 0)  /* the bid stood; the caller loses a die    */ \
    X(CAP_CALL_FALSE,      0, 0)  /* the bid fell; the bidder loses a die     */ \
    X(CAP_LOSES,           0, 0)  /* "Bo loses a die": {loss} in the two above,
                                     set apart on the screen (in blood)     */ \
    X(CAP_OUT,             0, 0)  /* "Bo is out"                              */ \
    X(CAP_WINS,            0, 0)  /* "Alex wins"                              */ \
    X(CAP_JOIN,            4, 0)  /* ". " between two clauses                 */ \
    X(CAP_INVITE,          0, 0)                                                 \
    X(CAP_JOINED,          0, 0)                                                 \
    X(CAP_LEFT,            0, 0)                                                 \
    /* a caption's shorter forms, said only when the long one is past       */ \
    /* CN_CAP_BUDGET (cn_say.h): the same sentence, its filler dropped      */ \
    X(CAP_START_SHORT,     0, 0)  /* "Alex bids first"                        */ \
    X(CAP_INVITE_SHORT,    0, 0)  /* "Alex wants a game": the picture names it*/ \
    X(CAP_CLIP,            1, 0)  /* the mark a clipped name ends in          */ \
    /* screen lines, drawn for one phone, so they may say "you"             */ \
    X(HEAD_OPEN,          36, 0)  /* my turn, no bid yet                      */ \
    X(HEAD_RAISE_OR_CALL, 36, 0)                                                 \
    X(HEAD_ONLY_CALL,     36, 0)  /* the top bid stands: only the call        */ \
    X(HEAD_THEIR_TURN,     0, 0)  /* "Bo's turn"                              */ \
    X(HEAD_YOU_WIN,       36, 0)                                                 \
    X(HEAD_WINS,           0, 0)                                                 \
    X(HEAD_YOU_OUT,       36, 0)                                                 \
    X(HEAD_STAGED_BID,     0, 0)  /* "Send to bid four 3s"                    */ \
    X(HEAD_STAGED_CALL,    0, 0)  /* "Send to call Liar on four 3s"           */ \
    X(SUB_STANDING,        0, 0)  /* "Bid to beat: four 3s by Alex"           */ \
    X(SUB_NONE,           36, 0)                                                 \
    X(SUB_TABLE,          36, 0)  /* "14 dice on the table"                   */ \
    X(REVEAL_COUNT,        0, 0)  /* "There were five"                        */ \
    X(BTN_RAISE,          12, 0)                                                 \
    X(BTN_CALL,           12, 0)  /* "Liar": the owner's word for the call     */ \
    X(BTN_JOIN,           12, 0)                                                 \
    X(BTN_START,          12, 0)                                                 \
    /* the lobby                                                            */ \
    X(LOBBY_ROW,           0, 0)  /* "2. Bo"                                  */ \
    X(LOBBY_ROW_YOU,       0, 0)  /* "2. Bo (You)"                            */ \
    X(LOBBY_WAITING,      36, 0)                                                 \
    X(LOBBY_FULL,         36, 0)                                                 \
    X(LOBBY_YOU,           8, 0)  /* "(you)", dim after my own name's row      */ \
    X(BTN_LEAVE,          12, 0)  /* get up from a lobby's seat (can_exit)    */ \
    /* errors                                                               */ \
    X(ERR_UNREADABLE,     36, 0)                                                 \
    X(ERR_NEWER,           0, 0)                                                 \
    X(ERR_DAMAGED,        36, 0)                                                 \
    /* the rules page                                                       */ \
    X(RULES_TITLE,        36, 0)                                                 \
    X(RULE_1,              0, 0)                                                 \
    X(RULE_2,              0, 0)                                                 \
    X(RULE_3,              0, 0)                                                 \
    X(RULE_4,              0, 0)                                                 \
    X(RULE_5,              0, 0)                                                 \
    X(RULE_6,              0, 0)                                                 \
    /* the host's fixed labels, read by key through cn_api_string          */ \
    X(NAME_PROMPT,        16, 0)  /* the nickname field's placeholder         */ \
    X(BTN_NEXT,           12, 0)  /* after a reveal: look at the next round   */ \
    X(STAMP_LOSES,        12, 0)  /* the loser's row at a reveal              */ \
    X(STAMP_OUT,           8, 0)  /* a seat with no dice                      */

#define CN_KEY_ENUM(name, max, empty) CN_K_##name,
typedef enum { CN_KEYS(CN_KEY_ENUM) CN_K_COUNT } CnKey;
#undef CN_KEY_ENUM

enum { CN_RULES_N = CN_K_RULE_6 - CN_K_RULE_1 + 1, CN_NUM_WORDS = 12 };

/* The key each slot answers to: the name a host and the generated modules
 * ask for. datagen reads this as dimension 0's labels. */
#define CN_KEY_LABEL(name, max, empty) [CN_K_##name] = #name,
static const char *const CN_KEY_NAME[CN_K_COUNT] = { CN_KEYS(CN_KEY_LABEL) };
#undef CN_KEY_LABEL

#endif
