#include "keys.h"
extern const char *const FS_STRINGS_EN[FS_K_COUNT];
extern const char *const FS_STRINGS_RU[FS_K_COUNT];
static inline const char *toy_text(int lang, int key) { return (lang ? FS_STRINGS_RU : FS_STRINGS_EN)[key]; }
static inline int toy_count(void) { return FS_K_COUNT; }
