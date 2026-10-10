/* malloc, calloc and free for a wasm32 kernel, as a bump allocator: every
 * allocation is the next 16-aligned bytes above __heap_base, linear memory
 * grows when they run out, and free does nothing. Declared in
 * include/stdlib.h.
 *
 * For a kernel that allocates a few long-lived buffers (a transposition
 * table, a scratch arena) and never returns them. A kernel that allocates and
 * frees in a loop will grow without bound on this and needs a real allocator.
 *
 * A request that cannot be met returns 0 and leaves the break and the memory
 * where they were. */
#include <stdlib.h>
#include <string.h>

#define WASM_PAGE 65536ull

extern unsigned char __heap_base;
static unsigned char *g_brk = 0;

#ifdef __wasm_atomics__
/* Built -matomics, the module runs as threads over one shared memory and every
 * thread allocates, so the break (and the memory.grow it drives) is
 * serialized. A one-byte spinlock; memory.grow itself is atomic on shared
 * memory, only the break needs guarding. Keyed on the compiler's own macro
 * rather than a product's flag: a threaded build cannot forget to ask. */
static unsigned char g_brk_lock = 0;
static void brk_lock(void)   { while (__atomic_exchange_n(&g_brk_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void brk_unlock(void) { __atomic_store_n(&g_brk_lock, 0, __ATOMIC_RELEASE); }
#else
static void brk_lock(void)   { }
static void brk_unlock(void) { }
#endif

void *malloc(size_t n)
{
    /* Rounding up to 16 would wrap, and a wrapped size is a small one. */
    if (n > (size_t)-1 - 15ul) return 0;
    size_t need = (n + 15ul) & ~15ul;
    brk_lock();
    if (!g_brk) g_brk = (unsigned char *)(((size_t)&__heap_base + 15ul) & ~15ul);
    /* Sizes, not pointers: `g_brk + need` wraps for a request near 4 GiB and
     * would compare as fitting. 64 bits wide, because a full 65536 pages is
     * itself one past what size_t holds. */
    unsigned long long brk = (size_t)g_brk;
    for (;;) {
        /* The end as THIS thread sees it. Over a shared memory that can be
         * behind the break: another thread grew the memory and allocated in
         * what it grew, and an engine may not have told this one yet. No room
         * then, not a negative amount of it, and the grow below is what brings
         * this thread's view up to date. */
        unsigned long long end = (unsigned long long)__builtin_wasm_memory_size(0) * WASM_PAGE;
        unsigned long long room = end > brk ? end - brk : 0;
        if (need <= room) break;
        /* Grow by what is missing, counting the room already above the break,
         * plus 2 pages of slack. Not more: a host is charged for the size of
         * the buffer, not for the pages touched, so headroom nobody asked for
         * is paid for by every instance. And without the slack when the slack
         * is what does not fit, so a request that fits under the module's
         * maximum is never refused. */
        unsigned long long missing = need - room;
        size_t grow_pages = (size_t)(missing / WASM_PAGE) + (missing % WASM_PAGE != 0);
        if (__builtin_wasm_memory_grow(0, grow_pages + 2ul) == (size_t)-1 &&
            __builtin_wasm_memory_grow(0, grow_pages) == (size_t)-1) { brk_unlock(); return 0; }
    }
    void *p = g_brk;
    g_brk += need;
    brk_unlock();
    return p;
}

void *calloc(size_t n, size_t sz)
{
    if (sz && n > (size_t)-1 / sz) return 0;
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *p) { (void)p; }
