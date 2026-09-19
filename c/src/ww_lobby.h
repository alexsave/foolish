// The lobby's rules, in the kernel.
//
// THE HOLE THIS SHAPE EXISTS TO CLOSE, and for this game it is fatal rather than
// merely unfair. The tree this was forked from used to deal a 1:1 game LIVE the
// moment it was created. The creator therefore saw their hand BEFORE anything
// committed, so tapping New game until the deck favoured them cost nothing and
// left no trace.
//
// Here the deal is not cards, it is ROLES. A creator who can see state before
// committing re-creates until they are not the wolf, or until they are, and there
// is no recovering from that: the game's whole premise is that nobody chose their
// role. So it has to be impossible by construction, not discouraged.
//
// ONE PATH FOR EVERY CHAT SHAPE. Creating LOCKS the seed and the game id and
// seals a WAITING bubble seating only the creator. Nobody has a role until Start.
// There is deliberately no "deal immediately in a 1:1" shortcut anywhere in this
// tree - that shortcut IS the bug, and a comment saying so is worth more than the
// convenience it would buy.
//
// NO PLAYER-COUNT PICKER. The count is decided AT START by who actually joined.
// An open lobby, not a configured one. That is also what makes the two Start
// routes equivalent: both deal ww_deal(locked_seed, joins.count), so the roles
// depend on the seed the lobby chain carries and the final join count, never on
// which route assembled the roster or when it was sealed.
//
// THE UI ONLY ASKS. Every question below is answered here and drawn by the view
// as it came back. A lobby control re-decided in Swift is a lobby control two
// clients can disagree about, and in this game one of those disagreements hands
// somebody a role.
#ifndef WW_LOBBY_H
#define WW_LOBBY_H

#include "ww_game.h"

// What a lobby offers this viewer. Exactly one of these, because the screen shows
// one control and a screen that shows two has a rule missing.
#define WW_LOBBY_WAITING  0   // seated, and there is nothing to do but wait
#define WW_LOBBY_JOIN     1   // not seated, and there is room
#define WW_LOBBY_START    2   // seated, and this table can be dealt
#define WW_LOBBY_INVITE   3   // seated and alone-ish: ask somebody else in
#define WW_LOBBY_FULL     4   // not seated, and there is no room
#define WW_LOBBY_TOO_FEW  5   // seated, but this CHAT can never seat enough

// How many seats this chat can hold. The one thing that varies by chat shape.
//
// A 1:1 chat holds exactly two people, so "all seats taken" has to read correctly
// once the single possible opponent has joined rather than "waiting for three
// more". And two is below this game's minimum, which has a consequence worth
// stating plainly rather than discovering: WEREWOLF CANNOT BE PLAYED IN A 1:1
// CHAT. A hidden-role game needs enough seats for the roles to mean anything -
// with two, the wolf and the villager both know everything - so the lobby says so
// (WW_LOBBY_TOO_FEW) instead of counting up to a Start that will never come.
int ww_lobby_capacity(int chat_is_dm);

// Can this table be dealt at all? The minimum is a LOBBY rule and lives here, so
// no client can reach Start with four players and deal one wolf against three
// villagers who already know the answer.
int ww_lobby_can_start(int joined, int capacity);

// Is this chat shape capable of ever starting? Separate from can_start because
// "not yet" and "not ever" are different sentences on the screen.
int ww_lobby_impossible(int capacity);

// How many more players before Start becomes possible. 0 once it is.
int ww_lobby_needs(int joined);

// May this viewer leave? Only somebody who is in, and only when leaving does not
// empty the lobby - the creator alone in their own invite has nothing to leave.
int ww_lobby_can_exit(int my_seat, int joined);

// THE one lobby decision. `my_seat` is this device's seat or -1;
// `i_sent_the_newest` is whether the bubble on screen is this device's own.
//
// The newest sender STANDS ASIDE while there is still room: a full table has
// nobody left to stand aside for, and otherwise the person who just posted would
// be asked to post again instead of letting the next player in.
int ww_lobby_offered(int my_seat, int joined, int capacity, int i_sent_the_newest);

// The lowest unclaimed seat, which is the only seat a join may take. Lowest-free
// rather than "the next index": it makes a roster's new seats its own tail in
// seat order, which is what lets two devices derive the same order of joins from
// two lobby snapshots without having seen the bubbles in between.
int ww_lobby_free_seat(const uint8_t *claimed_seats, int n_claimed);

// START: deal the LOCKED seed at the join count. The only way a WwGame is ever
// dealt from a lobby, so both UI routes (join-then-start and join-and-start) are
// the same deal by construction rather than by two code paths agreeing.
// Returns WW_OK, or WW_ECOUNT when `joined` is not a table this game can deal.
int ww_lobby_start(WwGame *g, const uint8_t seed[32], int joined);

#endif
