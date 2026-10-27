/* arena.c - see arena.h. */
#include "arena.h"

bool cc_arena_on_fire(int streak) { return streak >= CC_ARENA_FIRE; }

int cc_arena_points(int result, int streak, int plies) {
    if (result == CC_ARENA_DRAW && plies < CC_ARENA_DRAW_PLIES) return 0;
    int base = result == CC_ARENA_WIN ? 2 : result == CC_ARENA_DRAW ? 1 : 0;
    return cc_arena_on_fire(streak) ? 2 * base : base;
}

int cc_arena_streak(int result, int streak) {
    if (result != CC_ARENA_WIN) return 0;
    return streak < 255 ? streak + 1 : 255;
}

bool cc_arena_name_ok(const char *s, size_t n) {
    if (n < CC_ARENA_NAME_MIN || n > CC_ARENA_NAME_MAX || s[0] == ' ' || s[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) return false;
    }
    return true;
}
