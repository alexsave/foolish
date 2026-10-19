// migrate_merge.h - cbase-export's explicit TRANSFORM step, applied to the copy it makes (never to the source table).
//
// MERGE_CASE_DUPLICATES: accounts whose names compare equal (auth/names.h cb_name_key) are one person.
// Each group merges into its OLDEST account (created_at, then id): it keeps its id, history and display
// name, takes the password_hash of the NEWEST, and its email becomes the CURRENT transform of its name
// (v1_http.h cb_v1_email_of of cb_name_case: sign-in upper-cases whatever is typed, so any spelling
// reaches it). Every reference to a merged-away id moves to the kept id: game_members, games.roster
// (through the kernel's roster codec), replays.players, replay_players, chat. Sessions, refresh tokens,
// ratings and sign-in failures of the merged-away accounts are dropped. A game (or replay) left with
// one user in every human seat is deleted with its moves, members, bot links, chat, replays. Any
// other duplicate in a roster fails the transform. The unique username_key index is then rebuilt,
// which proves no group is left.
// RESET_RATINGS: every ratings row (accounts; bots are untouched) goes back to what a new account gets
// (TABLE_RATING_START, previous the same, no games). The rating stamps inside finished games' state
// blobs and replays are history and nothing recomputes current ratings from them.
#ifndef CB_MIGRATE_MERGE_H
#define CB_MIGRATE_MERGE_H

#include <stdio.h>

#define CB_MIG_MERGE_CASE_DUPLICATES 1
#define CB_MIG_RESET_RATINGS 2

typedef struct {
    long groups, merged_away, emails_changed, passwords_taken;
    long members_repointed, members_deduped, chat_repointed, rosters_repointed, replays_repointed, replay_players_repointed;
    long sessions_dropped, refresh_dropped, fails_dropped, ratings_dropped;
    long games_deleted, moves_deleted, members_deleted, bots_deleted, chat_deleted, replays_deleted, replay_players_deleted;
    long ratings_reset;
    long email_mismatch;   // other accounts whose email is not the current transform (informational)
    long unexpected;
} cb_mig_tx;

// CB_OK, or an error; the file is unchanged on any failure (one transaction). Reports to `log`.
int cb_mig_transform(const char *path, unsigned flags, FILE *log, cb_mig_tx *out);

#endif
