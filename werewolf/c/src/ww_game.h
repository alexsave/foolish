// The werewolf kernel: roles from a seed, one night record per living seat,
// a deterministic kill, and the day that follows.
//
// ONE IMPLEMENTATION, IN C, for the same reason the tree it was forked from
// kept one: every device replays the same bytes with no server in the loop, so
// two engines that disagree about anything here fork the game. There is
// nothing to port and nothing to keep in sync.
//
// WHAT THE NIGHT HAS TO HIDE, and why it is shaped the way it is. In a message
// thread the ACT OF SENDING is visible. Every previous attempt at this genre
// died there: a wolf who sends at night has announced himself, because nobody
// else had a reason to send. So the night is built so that sending carries no
// information at all:
//
//   * every living seat sends exactly one record per night, and the night does
//     not end until they all have;
//   * every seat picks another seat. A wolf's pick is a kill vote, the seer's
//     is a question, a villager's is who they dreamt about and does nothing.
//     Same screen, same taps, same deliberation;
//   * the wolves' line rides INSIDE that one record (WW_REC_HAS_CHAT), never as
//     a second message. A separate message is a countable bubble, and three
//     extra bubbles on a five-player night is three wolves. ONE BUBBLE PER
//     PLAYER PER NIGHT, always;
//   * what a third seat is allowed to see of a record is decided here and
//     rendered by ww_view.h, so no client can leak it by accident.
//
// The hiding is SOCIAL, not cryptographic - the same trust level as passing the
// phone around a table. The bytes of the envelope are visible to every device
// that receives it; what is defended is the READING, because that is what a
// player actually does. Padding the payload to a constant size was considered
// and rejected: anyone willing to count bytes could simply read the roles out
// of the same buffer instead, so padding defends against an attacker who does
// not exist while costing every real player URL budget.
#ifndef WW_GAME_H
#define WW_GAME_H

#include <stdint.h>

#define WW_MIN_PLAYERS 5
#define WW_MAX_PLAYERS 10

// A seat that is not a seat: no target chosen, no decider, no victim. 0xFF
// rather than -1 because every one of these crosses the wire as a byte.
#define WW_NO_SEAT 0xFF

// The wolf line's cap. Small on purpose: this rides inside the night record, so
// every byte here is URL budget every player pays every night. Long enough for
// "3 is asking too many questions", short enough that three wolves cost 135
// bytes of a ~375-byte ten-player night envelope.
#define WW_CHAT_MAX 40

// The game cannot outlast its own arithmetic: each night kills at most one and
// each day lynches at most one, so ten players are through in at most ten
// nights. The cap is a refusal, not a wrap.
#define WW_MAX_NIGHTS 10

// Every record the chain can ever hold: one per living seat per night, plus one
// lynch per day. Sized to the worst case rather than to a guess, so an overflow
// here means a bug in the caps above and not a long game.
#define WW_MAX_RECORDS (WW_MAX_NIGHTS * WW_MAX_PLAYERS + WW_MAX_NIGHTS)

enum WwRole {
    WW_ROLE_VILLAGER = 0,
    WW_ROLE_WOLF     = 1,
    WW_ROLE_SEER     = 2,
    WW_ROLE_UNKNOWN  = 0xFF   // what a view says when the viewer is not entitled
};

enum WwTeam {
    WW_TEAM_VILLAGE = 0,
    WW_TEAM_WOLVES  = 1,
    WW_TEAM_NONE    = 0xFF
};

enum WwPhase {
    WW_PHASE_LOBBY = 0,   // roster filling, no roles dealt
    WW_PHASE_NIGHT = 1,
    WW_PHASE_DAY   = 2,
    WW_PHASE_OVER  = 3
};

// Record flags.
//   WW_REC_AUTO_PASS - this record was not sent by its own seat. A later
//                      player carried it forward (see THE SKIP below). It never
//                      carries a target and never carries chat.
//   WW_REC_HAS_CHAT  - the wolf line rides in this record.
//   WW_REC_LYNCH     - a day record: the seat the table voted out.
#define WW_REC_AUTO_PASS 0x01
#define WW_REC_HAS_CHAT  0x02
#define WW_REC_LYNCH     0x04

typedef struct {
    uint8_t seat;
    uint8_t target;     // WW_NO_SEAT when there is no choice
    uint8_t night;      // the night (or day) this record belongs to
    uint8_t flags;      // WW_REC_*
    uint8_t chat_len;   // 0 unless WW_REC_HAS_CHAT
    char    chat[WW_CHAT_MAX];
} WwRecord;

typedef struct {
    uint8_t  phase;                       // WW_PHASE_*
    uint8_t  n_players;
    uint8_t  night;                       // index of the night/day in play
    uint16_t alive;                       // bit per seat
    uint8_t  role[WW_MAX_PLAYERS];        // WW_ROLE_*; the whole secret
    uint16_t turn;                        // accepted records - Rule P clause 2
    uint8_t  n_records;
    WwRecord rec[WW_MAX_RECORDS];
    uint8_t  victim[WW_MAX_NIGHTS];       // who the wolves took, per night
    uint8_t  lynched[WW_MAX_NIGHTS];      // who the table took, per day
    uint8_t  winner;                      // WW_TEAM_*, WW_TEAM_NONE while live
} WwGame;

// Return codes. Negative is a refusal, and the reason is the value: a caller
// that only checks for zero still cannot mistake a refusal for a pass.
#define WW_OK            0
#define WW_EPHASE      (-1)   // not the phase this action belongs to
#define WW_ESEAT       (-2)   // no such seat, or a dead one
#define WW_ETARGET     (-3)   // target is not a living other seat
#define WW_EDUP        (-4)   // this seat already has a record for this night
#define WW_ECHAT       (-5)   // chat from a non-wolf, or over WW_CHAT_MAX
#define WW_ECOUNT      (-6)   // a count out of range (players, nights, records)

// ---------------------------------------------------------------- the deal --

// How many wolves a table of n gets. The classic Mafia ladder (5-6: one, 7-9:
// two, 10: three) rather than a ratio, because the ratio's rounding is what
// makes a five-player game with two wolves unwinnable for the village: the
// first night takes the village to 4-2 and one bad lynch ends it before anyone
// has evidence.
int ww_wolf_count(int n_players);

// Deal roles from the 32-byte seed. Fisher-Yates over a role bag with
// deal_rng, which is the same crypto-grade stream the fork's deck shuffle used
// and for the same reason: a player legitimately observes SOME outputs of this
// stream (their own role), and a reversible generator would let them run it
// backwards and read the table.
//
// Returns WW_OK, or WW_ECOUNT for a player count outside 5..10.
int ww_deal(WwGame *g, const uint8_t seed[32], int n_players);

int ww_is_alive(const WwGame *g, int seat);
int ww_alive_count(const WwGame *g);
int ww_team_of(int role);

// --------------------------------------------------------------- the night --

// The night's rotation: living seats in seat order starting at
// (night % n_players) and wrapping. Writes up to WW_MAX_PLAYERS seats, returns
// how many.
//
// The rotation is NOT a turn order - the night is simultaneous and anyone may
// send at any moment. It exists for two things that must be identical on every
// device: which wolf makes tonight's call (ww_night_decider), and which seats a
// carrier is allowed to pass for (ww_night_act's `carry`). It advances each
// night so the same voice is never the one deciding.
int ww_night_order(const WwGame *g, int night, uint8_t *out);

// Tonight's deciding wolf: the LAST living wolf in tonight's rotation, so the
// call moves with the rotation and is never the same voice twice running - and
// the wolf who decides is the one who has read every other wolf's line.
// WW_NO_SEAT when no wolf is alive.
int ww_night_decider(const WwGame *g);

// Has this seat's record for the current night landed yet?
int ww_night_has_record(const WwGame *g, int seat);

// THE one night entry point. `seat` chooses `target` (WW_NO_SEAT for no
// choice), optionally carrying `chat` (wolves only - a non-wolf that tries is
// WW_ECHAT rather than a silently dropped field, because a client that thinks
// it sent a wolf line and did not is a client that will send a second bubble).
//
// THE SKIP. `carry` non-zero also auto-passes every living seat that PRECEDES
// this one in tonight's rotation and has not sent. No timer can exist here,
// because nobody has to be awake: the night cannot be held open waiting on one
// player, and waiting on exactly one player would announce that player anyway.
// So the night moves forward under its own participants, and the carry is
// forward-only - a seat that was passed for cannot be un-passed, and a seat
// LATER in the rotation is never passed for, so no single player can end the
// night alone.
//
// The one-minute gate on offering the carry is ww_may_carry below, and it is a
// gate on the UI, not on this: a chain that legitimately raced (two players
// carrying at once) must still replay on every device, and a kernel that
// refused a carry by the clock would make replay depend on when it ran.
//
// Resolves the night and advances to WW_PHASE_DAY when the last living seat's
// record lands.
int ww_night_act(WwGame *g, int seat, int target,
                 const char *chat, int chat_len, int carry);

// Accept ONE carried pass for `seat`. This is what ww_night_act's carry loop
// pushes through, and it is also how a replayed chain re-applies a pass that a
// previous device carried: the chain states WHICH seats were passed for, and
// replay applies exactly those rather than re-deriving them from the rotation.
// Re-deriving would make an old game's outcome depend on the rotation as the
// REPLAYING build computes it, so a rotation change would silently rewrite every
// game in flight.
int ww_night_pass_for(WwGame *g, int seat);

// ----------------------------------------------------------------- the day --

// The table votes `target` out and the next night begins (or the game ends).
// One record, because the argument is the part that happens in the thread; what
// the kernel needs is the verdict.
int ww_day_lynch(WwGame *g, int target);

// ------------------------------------------------------- clocks the UI owes --
//
// Both of these live here rather than in the extension because they are RULES,
// and a rule that lives in a view is a rule each client gets to have its own
// opinion about. Neither is an input to replay - see ww_night_act.

// Send is disabled for ten seconds after the night screen opens. This removes
// the only real latency tell left: an instant answer means you had no decision
// to make, and the only players with no decision to make are the villagers.
//
// TEN IS A GUESS. It is long enough to be felt and short enough not to be
// resented, but the number that is actually right is the one a real table
// settles, and nothing here has been played by humans yet.
#define WW_SEND_FLOOR_S 10

// How long before a player may carry the late. A minute, on the same footing
// as the floor: long enough that a table playing in real time never sees it,
// short enough that a correspondence game is never stuck.
#define WW_CARRY_AFTER_S 60

// Seconds left on the floor, given when the screen opened and what time it is
// now. Both are unix seconds mod 65536 - the width the envelope's clock carries
// - so the subtraction wraps correctly across the hour the counter rolls.
// Returns 0 when the floor is spent.
int ww_send_floor_remaining(uint16_t opened_at, uint16_t now);

// May this player carry the late yet? Same clock width, same wrap.
int ww_may_carry(uint16_t last_seal_at, uint16_t now);

#endif
