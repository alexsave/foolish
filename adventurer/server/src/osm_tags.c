// osm_tags.c - osm_tags.h.
#include "osm_tags.h"

#include <stdlib.h>
#include <string.h>

static const char *tag(const pbf_tag *tags, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcmp(tags[i].k, key) == 0) return tags[i].v;
    return NULL;
}

static bool is(const char *v, const char *want) { return v && strcmp(v, want) == 0; }
static bool in3(const char *v, const char *a, const char *b, const char *c) { return is(v, a) || is(v, b) || is(v, c); }

#define BF (ADV_ACC_BIKE | ADV_ACC_FOOT)

typedef struct {
    const char *highway;
    uint8_t     hclass;
    uint8_t     base;   // the access bits before the way's own signs are read
} hw_rule;

// Highway values that are roads, and what each is for before overrides. Anything not here
// (bus_stop, crossing, services, raceway, proposed, construction, platform, corridor, elevator,
// escape, bus_guideway, busway, and whatever is invented next) is not a road to explore.
static const hw_rule rules[] = {
    { "motorway", ADV_HC_MOTORWAY, 0 },          { "motorway_link", ADV_HC_MOTORWAY, 0 },
    { "trunk", ADV_HC_TRUNK, ADV_ACC_BIKE },     { "trunk_link", ADV_HC_TRUNK, ADV_ACC_BIKE },
    { "primary", ADV_HC_PRIMARY, BF },           { "primary_link", ADV_HC_PRIMARY, BF },
    { "secondary", ADV_HC_SECONDARY, BF },       { "secondary_link", ADV_HC_SECONDARY, BF },
    { "tertiary", ADV_HC_TERTIARY, BF },         { "tertiary_link", ADV_HC_TERTIARY, BF },
    { "residential", ADV_HC_RESIDENTIAL, BF },   { "unclassified", ADV_HC_UNCLASSIFIED, BF },
    { "living_street", ADV_HC_LIVING, BF },      { "road", ADV_HC_OTHER, BF },
    { "service", ADV_HC_SERVICE, BF },           { "track", ADV_HC_TRACK, BF },
    { "path", ADV_HC_PATH, ADV_ACC_FOOT },       { "bridleway", ADV_HC_PATH, ADV_ACC_FOOT },
    { "footway", ADV_HC_FOOTWAY, ADV_ACC_FOOT }, { "pedestrian", ADV_HC_PEDESTRIAN, ADV_ACC_FOOT },
    { "steps", ADV_HC_STEPS, ADV_ACC_FOOT },     { "cycleway", ADV_HC_CYCLEWAY, ADV_ACC_BIKE },
};

bool osm_way_road(const pbf_tag *tags, int n_tags, uint8_t *access, uint8_t *hclass, uint8_t *flags) {
    const char *hw = tag(tags, n_tags, "highway");
    if (!hw) return false;
    const hw_rule *r = NULL;
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++)
        if (strcmp(rules[i].highway, hw) == 0) { r = &rules[i]; break; }
    if (!r) return false;
    if (is(tag(tags, n_tags, "area"), "yes")) return false;   // a plaza drawn as a way is not a road
    const char *bicycle = tag(tags, n_tags, "bicycle"), *foot = tag(tags, n_tags, "foot");
    uint8_t acc = r->base;
    switch (r->hclass) {
    case ADV_HC_SERVICE: {
        const char *service = tag(tags, n_tags, "service");
        if (in3(service, "parking_aisle", "driveway", "drive-through")) return false;
        break;
    }
    case ADV_HC_PATH:   // path and bridleway: bikes when signed in
        if (is(bicycle, "yes") || is(bicycle, "designated")) acc |= ADV_ACC_BIKE;
        break;
    case ADV_HC_FOOTWAY: {
        // A sidewalk or a crossing is the street beside it drawn again a few metres over: the
        // same ground twice, and a matcher's trap (a runner on the street was matched onto the
        // sidewalk for whole stretches). The street carries the foot access; these are skipped.
        const char *footway = tag(tags, n_tags, "footway");
        if (in3(footway, "sidewalk", "crossing", "traffic_island")) return false;
        if (is(bicycle, "yes") || is(bicycle, "designated")) acc |= ADV_ACC_BIKE;
        break;
    }
    case ADV_HC_PEDESTRIAN:   // bikes only where allowed by name (steps: never)
        if (is(bicycle, "yes") || is(bicycle, "designated")) acc |= ADV_ACC_BIKE;
        break;
    case ADV_HC_CYCLEWAY:   // feet unless signed off
        if (!is(foot, "no")) acc |= ADV_ACC_FOOT;
        break;
    default: break;
    }
    // The way's own signs, after the kind: a legal "no" beats every default, a legal "yes" on a
    // closed road opens it to that type only.
    const char *access_tag = tag(tags, n_tags, "access");
    if (is(access_tag, "no") || is(access_tag, "private")) {
        uint8_t let_in = 0;
        if (in3(bicycle, "yes", "designated", "permissive")) let_in |= ADV_ACC_BIKE;
        if (in3(foot, "yes", "designated", "permissive")) let_in |= ADV_ACC_FOOT;
        acc &= let_in;
    }
    if (is(bicycle, "no")) acc &= (uint8_t)~ADV_ACC_BIKE;
    if (is(foot, "no")) acc &= (uint8_t)~ADV_ACC_FOOT;
    if (is(tag(tags, n_tags, "motorroad"), "yes")) acc = 0;
    if (!acc) return false;
    *access = acc;
    *hclass = r->hclass;
    *flags = 0;
    if (in3(tag(tags, n_tags, "oneway"), "yes", "1", "-1")) *flags |= ADV_SEGF_ONEWAY;
    return true;
}

// Copies s into out[cap], cut on a UTF-8 character boundary so a cut name is still valid text.
static void copy_name(char *out, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // never end inside a character
    }
    memcpy(out, s, n);
    out[n] = 0;
}

bool osm_relation_region(const pbf_tag *tags, int n_tags, int admin_level_wanted, char name[ADV_REGION_NAME]) {
    if (!is(tag(tags, n_tags, "boundary"), "administrative")) return false;
    const char *lvl = tag(tags, n_tags, "admin_level");
    if (!lvl || !*lvl) return false;
    char *end;
    long l = strtol(lvl, &end, 10);
    if (*end || l != admin_level_wanted) return false;
    const char *nm = tag(tags, n_tags, "name:en");
    if (!nm || !*nm) nm = tag(tags, n_tags, "name");
    if (!nm || !*nm) return false;
    copy_name(name, ADV_REGION_NAME, nm);
    return true;
}
