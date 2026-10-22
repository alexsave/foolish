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

// True for boundary=administrative at the wanted admin_level with a name; the name (name:en
// preferred, else name) is copied into name[], cut to fit ADV_REGION_NAME on a UTF-8 boundary.
bool osm_relation_region(const pbf_tag *tags, int n_tags, int admin_level_wanted, char name[ADV_REGION_NAME]);

#endif
