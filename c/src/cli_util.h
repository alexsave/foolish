// Shared command-line helpers for the cnitro_* main programs. Header-only
// (static inline) so there is nothing extra to link. Replaces the get_arg /
// parse_int copies that used to live in every main_*.c.
#ifndef CNITRO_CLI_UTIL_H
#define CNITRO_CLI_UTIL_H

#include <stdlib.h>
#include <string.h>

// Look up "--key=value" in argv; returns the value, or `def` if absent.
static inline const char *get_arg(int argc, char **argv,
                                  const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') {
            return argv[i] + 2 + kl + 1;
        }
    }
    return def;
}

// Is a bare "--key" present? get_arg only matches "--key=value", so a flag
// spelled the way a flag is spelled reads as absent through it - which is how
// --quiet came to be silently ignored in more than one main_*.c. "--key=N"
// reads N, so a script can pass the flag with a computed value and "--key=0"
// turns it off.
static inline int has_flag(int argc, char **argv, const char *key) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) != 0 || strncmp(argv[i] + 2, key, kl) != 0) continue;
        char c = argv[i][2 + kl];
        if (c == 0) return 1;
        if (c == '=') return atoi(argv[i] + 3 + kl) != 0;
    }
    return 0;
}

// Parse an integer, falling back to `def` for a NULL string.
static inline int parse_int(const char *s, int def) {
    return s ? atoi(s) : def;
}

#endif
