// wire_table.h - EVERY FIELD OF THE BINARY SURFACE (wire.h), once, as data: the layout hash
// (wire.c) and the TypeScript decoder (tools/wiregen.c) both read this table, so neither can
// drift from the structs. Offsets and sizes are the compiler's (offsetof, sizeof).
#ifndef ADV_WIRE_TABLE_H
#define ADV_WIRE_TABLE_H

#include "wire.h"

#include <stddef.h>

// Field kinds: how the decoder reads the bytes.
enum { WK_U8, WK_U16, WK_U32, WK_I32, WK_F32, WK_I64, WK_U64, WK_STR };

typedef struct {
    const char *strct;   // "adv_w_user"
    const char *field;   // "unique_m"
    size_t      off, size;
    int         kind;
    size_t      elem;    // one element's size (size / elem = the array's length; 1 for a scalar)
} adv_wire_field;

typedef struct {
    const char *name;
    size_t      size;
} adv_wire_struct;

#define F(S, f, k, e) { #S, #f, offsetof(S, f), sizeof(((S *)0)->f), k, e }

static const adv_wire_struct ADV_WIRE_STRUCTS[] = {
    { "adv_w_list", sizeof(adv_w_list) },
    { "adv_w_user_def", sizeof(adv_w_user_def) },
    { "adv_w_user", sizeof(adv_w_user) },
    { "adv_w_region", sizeof(adv_w_region) },
    { "adv_w_user_stats", sizeof(adv_w_user_stats) },
    { "adv_w_region_progress", sizeof(adv_w_region_progress) },
    { "adv_w_leader", sizeof(adv_w_leader) },
    { "adv_w_leaders", sizeof(adv_w_leaders) },
    { "adv_w_activity", sizeof(adv_w_activity) },
    { "adv_w_accepted", sizeof(adv_w_accepted) },
    { "adv_w_ops", sizeof(adv_w_ops) },
};

static const adv_wire_field ADV_WIRE_FIELDS[] = {
    F(adv_w_list, hash, WK_U32, 4), F(adv_w_list, n, WK_U32, 4),

    F(adv_w_user_def, id, WK_U32, 4), F(adv_w_user_def, kind, WK_U8, 1), F(adv_w_user_def, name_len, WK_U8, 1),
    F(adv_w_user_def, home_lat, WK_I32, 4), F(adv_w_user_def, home_lon, WK_I32, 4), F(adv_w_user_def, name, WK_STR, 1),

    F(adv_w_user, id, WK_U32, 4), F(adv_w_user, kind, WK_U8, 1), F(adv_w_user, name_len, WK_U8, 1),
    F(adv_w_user, n_activities, WK_U32, 4), F(adv_w_user, version, WK_U32, 4), F(adv_w_user, unique_m, WK_F32, 4),
    F(adv_w_user, home_lat, WK_I32, 4), F(adv_w_user, home_lon, WK_I32, 4), F(adv_w_user, name, WK_STR, 1),

    F(adv_w_region, id, WK_U16, 2), F(adv_w_region, name_len, WK_U8, 1), F(adv_w_region, len_m, WK_F32, 4),
    F(adv_w_region, c_lat, WK_I32, 4), F(adv_w_region, c_lon, WK_I32, 4), F(adv_w_region, min_lat, WK_I32, 4),
    F(adv_w_region, min_lon, WK_I32, 4), F(adv_w_region, max_lat, WK_I32, 4), F(adv_w_region, max_lon, WK_I32, 4),
    F(adv_w_region, name, WK_STR, 1),

    F(adv_w_user_stats, hash, WK_U32, 4), F(adv_w_user_stats, id, WK_U32, 4), F(adv_w_user_stats, version, WK_U32, 4),
    F(adv_w_user_stats, n_activities, WK_U32, 4), F(adv_w_user_stats, n_regions, WK_U32, 4),
    F(adv_w_user_stats, n_super_unique_segs, WK_U32, 4), F(adv_w_user_stats, unique_m, WK_F32, 4),
    F(adv_w_user_stats, super_unique_m, WK_F32, 4), F(adv_w_user_stats, last_activity_ms, WK_I64, 8),
    F(adv_w_user_stats, first_activity_ms, WK_I64, 8),

    F(adv_w_region_progress, region, WK_U16, 2), F(adv_w_region_progress, achievements, WK_U8, 1),
    F(adv_w_region_progress, len_m, WK_F32, 4), F(adv_w_region_progress, month_m, WK_F32, 4),
    F(adv_w_region_progress, year_m, WK_F32, 4), F(adv_w_region_progress, rank_month, WK_U8, 1),
    F(adv_w_region_progress, rank_year, WK_U8, 1),

    F(adv_w_leader, user, WK_U32, 4), F(adv_w_leader, m, WK_F32, 4),
    F(adv_w_leaders, hash, WK_U32, 4), F(adv_w_leaders, region, WK_U16, 2),
    // The two 3x3 leader tables are read as 9 adv_w_leader records each (an array of structs: the
    // decoder reads `elem`-sized records at off + i*elem).
    { "adv_w_leaders", "month", offsetof(adv_w_leaders, month), sizeof(((adv_w_leaders *)0)->month), WK_U64, sizeof(adv_w_leader) },
    { "adv_w_leaders", "year", offsetof(adv_w_leaders, year), sizeof(((adv_w_leaders *)0)->year), WK_U64, sizeof(adv_w_leader) },

    F(adv_w_activity, kind, WK_U8, 1), F(adv_w_activity, type, WK_U8, 1), F(adv_w_activity, flags, WK_U8, 1),
    F(adv_w_activity, reason, WK_U8, 1), F(adv_w_activity, user, WK_U32, 4), F(adv_w_activity, activity, WK_U32, 4),
    F(adv_w_activity, gen_id, WK_U32, 4), F(adv_w_activity, n_points, WK_U32, 4), F(adv_w_activity, matched_m, WK_F32, 4),
    F(adv_w_activity, new_m, WK_F32, 4), F(adv_w_activity, accuracy, WK_F32, 4), F(adv_w_activity, false_m, WK_F32, 4),
    F(adv_w_activity, cpu_us, WK_U32, 4), F(adv_w_activity, e2e_us, WK_U32, 4), F(adv_w_activity, queue_live, WK_U32, 4),
    F(adv_w_activity, queue_backfill, WK_U32, 4), F(adv_w_activity, at_ms, WK_I64, 8),

    F(adv_w_accepted, hash, WK_U32, 4), F(adv_w_accepted, activity, WK_U32, 4), F(adv_w_accepted, queue_live, WK_U32, 4),
    F(adv_w_accepted, queue_backfill, WK_U32, 4),

    F(adv_w_ops, kind, WK_U8, 1), F(adv_w_ops, hash, WK_U32, 4), F(adv_w_ops, at_ms, WK_I64, 8), F(adv_w_ops, uptime_s, WK_U32, 4),
    F(adv_w_ops, users, WK_U32, 4), F(adv_w_ops, activities, WK_U32, 4), F(adv_w_ops, rejected, WK_U32, 4),
    F(adv_w_ops, queue_live, WK_U32, 4), F(adv_w_ops, queue_backfill, WK_U32, 4), F(adv_w_ops, act_per_s, WK_F32, 4),
    F(adv_w_ops, tiles_per_s, WK_F32, 4), F(adv_w_ops, act_cpu_p50_us, WK_U32, 4), F(adv_w_ops, act_cpu_p99_us, WK_U32, 4),
    F(adv_w_ops, tile_p50_us, WK_U32, 4), F(adv_w_ops, tile_p99_us, WK_U32, 4), F(adv_w_ops, e2e_p50_us, WK_U32, 4),
    F(adv_w_ops, e2e_p99_us, WK_U32, 4), F(adv_w_ops, rss_mb, WK_U32, 4), F(adv_w_ops, feed_conns, WK_U32, 4),
    F(adv_w_ops, http_conns, WK_U32, 4), F(adv_w_ops, journal_bytes, WK_U64, 8), F(adv_w_ops, accuracy_avg, WK_F32, 4),
    F(adv_w_ops, segs, WK_U32, 4), F(adv_w_ops, regions, WK_U32, 4), F(adv_w_ops, total_len_km, WK_F32, 4),
    F(adv_w_ops, threads, WK_U32, 4), F(adv_w_ops, recovered, WK_U32, 4),
};
#undef F

#define ADV_WIRE_N_STRUCTS (sizeof ADV_WIRE_STRUCTS / sizeof ADV_WIRE_STRUCTS[0])
#define ADV_WIRE_N_FIELDS (sizeof ADV_WIRE_FIELDS / sizeof ADV_WIRE_FIELDS[0])

// FNV-1a over every struct's name and size and every field's name, offset, size, kind and element
// size: the one hash both sides compute.
static inline uint32_t adv_wire_table_hash(void) {
    uint32_t h = 2166136261u;
#define MIX(v) do { uint64_t x_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { h ^= (uint8_t)(x_ >> (8 * b_)); h *= 16777619u; } } while (0)
#define MIXS(s) do { for (const char *c_ = (s); *c_; c_++) { h ^= (uint8_t)*c_; h *= 16777619u; } h ^= 0xFF; h *= 16777619u; } while (0)
    for (size_t i = 0; i < ADV_WIRE_N_STRUCTS; i++) { MIXS(ADV_WIRE_STRUCTS[i].name); MIX(ADV_WIRE_STRUCTS[i].size); }
    for (size_t i = 0; i < ADV_WIRE_N_FIELDS; i++) {
        const adv_wire_field *f = &ADV_WIRE_FIELDS[i];
        MIXS(f->strct); MIXS(f->field); MIX(f->off); MIX(f->size); MIX(f->kind); MIX(f->elem);
    }
#undef MIX
#undef MIXS
    return h ? h : 1;
}

#endif
