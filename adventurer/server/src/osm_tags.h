// osm_tags.h - which OSM ways are roads we score, and which relations are regions: pure
// functions over an entity's tags, so the rules are one table that a unit test can walk without
// a file.
//
// The rule is Wandrer's: a way counts for an activity type when that type may LEGALLY travel it.
// Motorways count for nobody, trunks for bikes only, ordinary roads for both, paths for feet
// (and bikes when signed), cycleways for bikes (and feet unless signed off), steps for feet.
// Private and no-access ways are not roads to explore unless bikes or feet are let in by name;
// driveways and parking aisles never are (they are somebody's yard, not the public network).
#ifndef ADV_OSM_TAGS_H
#define ADV_OSM_TAGS_H

#include "graph.h"
#include "pbf.h"

#include <stdbool.h>
#include <stdint.h>

// True when the way is a road we keep; then *access (ADV_ACC_*), *hclass (ADV_HC_*) and *flags
// (ADV_SEGF_*) say how. False: not a highway, a kind we skip, or nobody may use it.
bool osm_way_road(const pbf_tag *tags, int n_tags, uint8_t *access, uint8_t *hclass, uint8_t *flags);

// The way's road name (name:en preferred, else name), or NULL.
const char *osm_way_name(const pbf_tag *tags, int n_tags);

// The admin_level of a named boundary=administrative relation when bit admin_level of `levels`
// is set (bit 8 for towns, bits 7 and 8 for boroughs and villages both), else 0; the name
// (name:en preferred, else name) is copied into name[], cut to fit ADV_REGION_NAME on a UTF-8
// boundary. Levels run 1..ADV_REGION_MAX_LEVEL.
int osm_relation_region(const pbf_tag *tags, int n_tags, uint32_t levels, char name[ADV_REGION_NAME]);
// The relation's population= tag as a count: plain digits, with ',' or '.' or ' ' as thousands
// separators ("1,694,251"), at most 4e9; 0 for none or anything else ("approx 5000", "1.2e6").
uint32_t osm_population(const pbf_tag *tags, int n_tags);
// Whether the relation is a signed cycle route (type=route, route=bicycle): its member ways are
// flagged ADV_SEGF_BIKEROUTE, what the fake riders seek (fake.h).
bool osm_relation_bike_route(const pbf_tag *tags, int n_tags);

#endif
