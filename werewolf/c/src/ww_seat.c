#include "ww_seat.h"

static int name_is(const WwJoin *j, const char *name, int name_len) {
    if (j->name_len != (uint8_t)name_len) return 0;
    for (int i = 0; i < name_len; i++) if (j->name[i] != name[i]) return 0;
    return 1;
}

int ww_name_taken(const WwJoin *joins, int n, const char *name, int name_len) {
    if (!joins || n <= 0 || !name || name_len < 0 || name_len > WW_NAME_MAX) return 0;
    for (int i = 0; i < n; i++) if (name_is(&joins[i], name, name_len)) return 1;
    return 0;
}

int ww_seat_claimed_by_name(const WwJoin *joins, int n, const char *name, int name_len) {
    if (!joins || n <= 0 || !name || name_len <= 0 || name_len > WW_NAME_MAX) return -1;
    for (int i = 0; i < n; i++)
        if (name_is(&joins[i], name, name_len)) return joins[i].seat;
    return -1;
}

int ww_seat_cache_disowned(const WwJoin *joins, int n, int cached_seat,
                           const char *name, int name_len) {
    if (!joins || n <= 0 || cached_seat < 0 || !name || name_len <= 0) return 0;
    for (int i = 0; i < n; i++) {
        if (joins[i].seat != (uint8_t)cached_seat) continue;
        return !name_is(&joins[i], name, name_len);   // listed, under another name
    }
    return 0;   // this bubble names nobody at that seat - stay permissive
}

int ww_seat_resolve(int cached_seat, int sender_is_local, int n_players,
                    int last_actor_seat, int chat_is_dm) {
    if (cached_seat >= 0 && cached_seat < n_players) return cached_seat;
    if (sender_is_local && last_actor_seat >= 0 && last_actor_seat < n_players)
        return last_actor_seat;
    // A 1:1 chat of two has exactly one other person in it, so the receiver is
    // the seat the last actor is not. Werewolf never has a table of two, so this
    // branch is unreachable at 5..10 - it is kept because the rule it encodes is
    // about the CHAT and not the game, and deleting it would make the next
    // product that forks this re-derive it.
    if (chat_is_dm && n_players == 2 && last_actor_seat >= 0 && last_actor_seat < 2)
        return 1 - last_actor_seat;
    return -1;
}

// THE ONE SEAT DECISION, in the order its three name-aware steps have to be
// asked. `require_listed` is the ONLY difference between the board's answer and
// the lobby's, and it is a real difference in the rules rather than a caller's
// convenience. Written once so the two cannot drift.
static int resolve_named(const WwJoin *joins, int n_joins,
                         int cached_seat, int sender_is_local, int n_players,
                         int last_actor_seat, int chat_is_dm,
                         const char *name, int name_len, int require_listed) {
    // Name recovery first: the seat carrying MY claim name in THIS roster is my
    // seat here, even when a fork race left the numeric cache on a lost claim.
    const int by_name = ww_seat_claimed_by_name(joins, n_joins, name, name_len);
    if (by_name >= 0) return by_name;

    const int cached = ww_seat_cache_disowned(joins, n_joins, cached_seat, name, name_len)
                     ? -1 : cached_seat;
    const int seat = ww_seat_resolve(cached, sender_is_local, n_players,
                                    last_actor_seat, chat_is_dm);
    if (seat < 0 || !require_listed) return seat;
    for (int i = 0; i < n_joins; i++) {
        if (joins[i].seat != (uint8_t)seat) continue;
        // LISTED IS NOT THE SAME AS MINE, and in the fork that gap was a real
        // bug: a player who left a lobby was handed the seat of the player who
        // stayed, and could change the rules and deal as them. In a lobby a
        // NAMED device gets its seat by name or not at all - every lobby seat
        // carries a name, so "none of these rows is me" is a complete answer,
        // and the nameless inference above must not overrule it.
        if (name && name_len > 0)
            return name_is(&joins[i], name, name_len) ? seat : -1;
        return seat;
    }
    return -1;   // resolved, but this bubble's roster does not list it
}

int ww_seat_resolve_on_board(const WwJoin *joins, int n_joins,
                             int cached_seat, int sender_is_local, int n_players,
                             int last_actor_seat, int chat_is_dm,
                             const char *name, int name_len) {
    return resolve_named(joins, n_joins, cached_seat, sender_is_local, n_players,
                         last_actor_seat, chat_is_dm, name, name_len, 0);
}

int ww_seat_resolve_in_lobby(const WwJoin *joins, int n_joins,
                             int cached_seat, int sender_is_local, int n_players,
                             int last_actor_seat, int chat_is_dm,
                             const char *name, int name_len) {
    return resolve_named(joins, n_joins, cached_seat, sender_is_local, n_players,
                         last_actor_seat, chat_is_dm, name, name_len, 1);
}
