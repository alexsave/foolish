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

// Parse an integer, falling back to `def` for a NULL string.
static inline int parse_int(const char *s, int def) {
    return s ? atoi(s) : def;
}

#endif
