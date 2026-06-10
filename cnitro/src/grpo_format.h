// On-disk shard format for SFT / GRPO tuple corpora.
//
// Design choices:
//   * Records store the RAW observable game state from one seat's POV, not
//     an encoded float vector. Encoder bugs and feature changes don't
//     invalidate the corpus — only `calculate_legal_moves` semantics do.
//   * Chosen moves are stored as semantic identity (type + cards + cover
//     targets). The dataloader recomputes the legal-move set at training
//     time and matches the chosen move into it to recover `chosen_idx`.
//   * One file per worker thread; no shared locks during writing. A
//     manifest file (separate) lists shards and their bucket histograms.
//   * Header at offset 0 (32 bytes, fixed); footer appended at finalize
//     time carrying the CRC32 and final tuple/byte counts.
//
// All multi-byte fields are little-endian. macOS arm/x86 are both LE so
// we use direct memcpy/fwrite; a portable reader on a BE host would need
// byte-swaps but that's outside the current scope.
#ifndef CNITRO_GRPO_FORMAT_H
#define CNITRO_GRPO_FORMAT_H

#include "card.h"
#include "game.h"
#include "legal.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// --- Magic numbers + version -----------------------------------------------

// Header magic spelled "SPRG" on disk (LE encoding of 0x47525053 = "GRPS").
#define GRPO_SHARD_MAGIC   0x47525053u
#define GRPO_FOOTER_MAGIC  0x46545253u   // "SRTF" on disk
#define GRPO_SHARD_VERSION 1u

// --- Typed enums (replaces #defines in grpo_encode.h) ----------------------

typedef enum {
    GRPO_ROLE_ATTACKER    = 0,
    GRPO_ROLE_DEFENDER    = 1,
    GRPO_ROLE_CO_ATTACKER = 2,
    GRPO_ROLE_IDLE        = 3,
    GRPO_ROLE_COUNT       = 4,
} GrpoRole;

typedef enum {
    GRPO_DECK_VARIANT_36 = 0,   // 2-5p, values 5..13 (face 6..A)
    GRPO_DECK_VARIANT_52 = 1,   // 6-8p, values 1..13 (face 2..A)
    GRPO_DECK_VARIANT_COUNT = 2,
} GrpoDeckVariant;

GrpoDeckVariant grpo_deck_variant_for(int num_players);

// --- Shard header / footer -------------------------------------------------

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;            // GRPO_SHARD_MAGIC
    uint32_t version;          // GRPO_SHARD_VERSION
    uint32_t worker_id;
    uint32_t wall_time_unix;
    uint32_t base_seed;        // worker's base seed (per-game seeds derive from this)
    uint32_t reserved0;
    uint64_t reserved1;
} GrpoShardHeader;             // 32 bytes

typedef struct {
    uint32_t magic;            // GRPO_FOOTER_MAGIC
    uint32_t crc32;            // CRC32 of the record stream (header + footer excluded)
    uint64_t tuple_count;
    uint64_t stream_bytes;
} GrpoShardFooter;             // 24 bytes
#pragma pack(pop)

_Static_assert(sizeof(GrpoShardHeader) == 32, "header size");
_Static_assert(sizeof(GrpoShardFooter) == 24, "footer size");

// --- ObservableState -------------------------------------------------------
//
// Captures everything `calculate_legal_moves(g, self_idx, ...)` needs PLUS
// the public information the policy encoder consumes. After load, the
// dataloader reconstructs a `Game` with these fields populated, sets
// `g->players[self_idx].hand` to the stored hand, and zero-fills hidden
// fields (other players' hands, deck contents). That's enough state for the
// legal-move enumerator and the encoder.

#define GRPO_DISCARD_BITSET_BYTES 7   // 52 bits → 7 bytes

typedef struct {
    int8_t  num_players;
    int8_t  self_idx;
    int8_t  power_suit;
    int8_t  defender;
    int8_t  first_attacker;
    int8_t  num_battles;
    int8_t  num_eliminated;
    uint8_t good_players_mask;     // num_players <= 8 fits
    bool    has_flipped;
    Card    flipped;               // valid iff has_flipped
    int16_t deck_count;

    int8_t  hand_count;
    Card    hand[MAX_HAND_SIZE];

    Battle  table_battles[MAX_BATTLES];

    int8_t  player_status[MAX_PLAYERS];
    int8_t  player_hand_count[MAX_PLAYERS];
    bool    player_awaiting_attack[MAX_PLAYERS];
    int8_t  elimination_order[MAX_PLAYERS];

    uint8_t discard_bitset[GRPO_DISCARD_BITSET_BYTES];
    // Bitsets for each player slot (self's slot is zero-filled; kept for
    // straightforward indexing).
    uint8_t opp_held_bitset[MAX_PLAYERS][GRPO_DISCARD_BITSET_BYTES];
} ObservableState;

// --- TupleRecord -----------------------------------------------------------

typedef struct {
    uint32_t        game_seed;
    uint16_t        game_decision_idx;   // 0-based ordinal in the game
    GrpoRole        role;
    GrpoDeckVariant deck_variant;
    int8_t          n_live_at_decision;  // count of IN players (incl. self)
    ObservableState state;
    LegalMove       chosen_move;         // semantic identity of the SFT label
} TupleRecord;

// --- Builders --------------------------------------------------------------

// Populate an ObservableState from `g` as seat `self_idx` would observe it.
// Only reads publicly-observable game fields and self's own hand; never
// peeks at LOG_DRAW events or other players' hand contents.
void grpo_observable_state_build(const Game *g, int self_idx, ObservableState *out);

// Reconstruct enough of a Game from an ObservableState for both
// calculate_legal_moves(g, state->self_idx, ...) and grpo_encode_state to
// produce the same output they would have on the original game.
//
// Approach: copy public fields directly; populate g->players[self_idx].hand
// from state->hand; synthesize fake LOG_DISCARD (one per discard bit) and
// LOG_PICKUP (one per opp_held bit, attributed to each opp seat) events so
// the encoder's log-scan derivations recover the stored bitsets. Hidden
// fields (other players' hand contents, deck contents) stay zero — neither
// the legal enumerator nor the encoder needs them.
void grpo_state_to_game(const ObservableState *s, Game *g_out);

// Find the index of `chosen` within `moves`, comparing semantic identity
// (action_type + cards + attack_cards). Returns -1 if not found, which
// indicates either a legal-move enumeration change between collection time
// and load time, or a serialization bug.
int grpo_legal_move_match(const LegalMoves *moves, const LegalMove *chosen);

// Populate a TupleRecord. `chosen_move` should be the move the strategy
// actually played at this decision. `game_seed` and `game_decision_idx` are
// caller-supplied (the collector tracks both).
void grpo_tuple_build(const Game *g, int self_idx,
                      const LegalMove *chosen_move,
                      uint32_t game_seed, uint16_t game_decision_idx,
                      int n_live_at_decision,
                      TupleRecord *out);

// --- Shard writer ----------------------------------------------------------

// 1MB write buffer per worker. Flushed when full or at finalize.
#define GRPO_SHARD_BUF_BYTES (1u << 20)

typedef struct {
    FILE    *fp;
    uint8_t *buf;
    size_t   buf_n;
    uint32_t crc;          // running CRC32 of bytes appended to the stream
    uint64_t tuple_count;
    uint64_t stream_bytes; // total bytes already flushed + currently buffered
    uint32_t worker_id;
    uint32_t base_seed;
} GrpoShardWriter;

bool grpo_shard_open(GrpoShardWriter *w, const char *path,
                     uint32_t worker_id, uint32_t base_seed);
void grpo_shard_append(GrpoShardWriter *w, const TupleRecord *t);
bool grpo_shard_close(GrpoShardWriter *w);   // flushes, writes footer, fcloses

// --- Shard reader (used by SFT training + 10K-game smoke verifier) ---------

typedef struct {
    FILE *fp;
    GrpoShardHeader header;
    GrpoShardFooter footer;
    uint32_t crc_running;
    uint64_t tuples_read;
} GrpoShardReader;

bool grpo_shard_reader_open(GrpoShardReader *r, const char *path);
// Returns true if a record was read; false on EOF or error.
bool grpo_shard_reader_next(GrpoShardReader *r, TupleRecord *out);
// After all records consumed, finalize: verifies CRC and counts.
// Returns true if everything checks out.
bool grpo_shard_reader_close(GrpoShardReader *r);

// --- CRC32 (zlib polynomial 0xEDB88320) ------------------------------------

uint32_t grpo_crc32(uint32_t seed, const void *data, size_t n);

// --- Helpers ---------------------------------------------------------------

const char *grpo_role_name(GrpoRole r);

#endif
