/* bump_alloc_test.c - the bump allocator (bump_alloc.c), run as wasm.
 *
 * It cannot run natively: the allocator IS __heap_base and memory.grow. So
 * the checks are compiled into a module of their own, linked with a 2-page
 * initial memory and a 16-page maximum, and bump_alloc_test() returns 0 or
 * the line of the first check that failed. bump_alloc_test.mjs instantiates
 * it and reads that number; foolish/c's `make wasm-alloc-test` builds it
 * twice, plain and -matomics over a shared memory, and runs both.
 *
 * That part is one thread; in the -matomics build it takes the lock on every
 * call, which proves the lock is released on every path, the failing ones
 * included. The second part is the -matomics build only and is real threads:
 * bump_alloc_hammer() below, entered by several workers at once. */
#include <stdlib.h>
#include <string.h>

#define PAGE 65536ul
#define MAX_PAGES 16ul
#define MUST(c) do { if (!(c)) return __LINE__; } while (0)

extern unsigned char __heap_base;
static size_t pages(void) { return __builtin_wasm_memory_size(0); }

__attribute__((export_name("bump_alloc_test")))
int bump_alloc_test(void)
{
    /* The first allocation is the heap's first 16-aligned byte. */
    unsigned char *a = malloc(1);
    MUST(a && (size_t)a % 16 == 0);
    MUST(a >= &__heap_base && a < &__heap_base + 16);

    /* Each one is the next 16-aligned bytes: sizes round up, nothing overlaps. */
    unsigned char *b = malloc(17);
    unsigned char *c = malloc(16);
    MUST(b == a + 16);
    MUST(c == b + 32);

    /* free returns nothing, so the next allocation is not the freed one. */
    free(b);
    unsigned char *d = malloc(16);
    MUST(d == c + 16);

    /* What does not fit grows the memory by the pages that are missing and
     * two of slack. Most of a page is free above the break, so 3 pages and a
     * byte are missing 3 pages, not 4. */
    MUST(pages() == 2);
    MUST(2 * PAGE - (size_t)d - 16 >= 16);
    size_t n = 3 * PAGE + 1;
    unsigned char *big = malloc(n);
    MUST(big == d + 16);
    MUST(pages() == 2 + 3 + 2);
    MUST((size_t)big + n <= pages() * PAGE);
    big[n - 1] = 1;

    /* What fits does not grow it. */
    unsigned char *fits = malloc(PAGE);
    MUST(fits == big + ((n + 15) & ~15ul));
    MUST(pages() == 7);

    /* calloc zeroes: the bytes it is about to hand out are dirtied first. */
    unsigned char *next = fits + PAGE;
    memset(next, 0xAA, 64);
    unsigned char *z = calloc(4, 16);
    MUST(z == next);
    for (int i = 0; i < 64; i++) MUST(z[i] == 0);
    next += 64;

    /* A product that overflows size_t is refused, not wrapped to a small one. */
    MUST(calloc(0x10000ul, 0x10000ul) == 0);
    MUST(calloc(0x80000000ul, 2) == 0);
    MUST(calloc((size_t)-1, (size_t)-1) == 0);

    /* So is a size that wraps when rounded up, and one whose end wraps the
     * address space. */
    MUST(malloc((size_t)-1) == 0);
    MUST(malloc((size_t)-16) == 0);
    MUST(malloc((size_t)0 - (size_t)next) == 0);
    MUST(malloc((size_t)0 - (size_t)next + PAGE) == 0);

    /* Past the module's maximum the answer is 0, with the memory as it was. */
    MUST(malloc(MAX_PAGES * PAGE) == 0);
    MUST(pages() == 7);

    /* None of the refusals moved the break. */
    unsigned char *after = malloc(1);
    MUST(after == next);

    /* A request that fits exactly under the maximum is met, though its two
     * pages of slack would not fit; then there is nothing left. */
    size_t room = pages() * PAGE - (size_t)after - 16;
    MUST(malloc(room + (MAX_PAGES - 7) * PAGE) == after + 16);
    MUST(pages() == MAX_PAGES);
    MUST(malloc(1) == 0);
    return 0;
}

#ifdef __wasm_atomics__
/* ---- threads ----------------------------------------------------------------
 * Several workers enter bump_alloc_hammer() over one shared memory, each on a
 * stack the runner allocated for it, and park at the gate so that they
 * allocate at the same moment and not one after another.
 *
 * The gate is opened by the runner's own instance, and not before it has
 * allocated 40 pages: the memory has grown and the break has moved far past
 * the end every parked thread last saw. That is the state a threaded product
 * is in when its control thread allocates and then wakes its workers, and a
 * parked thread is the one an engine has had no chance to tell. So the first
 * thing a woken thread does is ask for 8 pages, which are not there: an
 * allocator that measured its room against the end it remembers would find
 * plenty, grow nothing, and hand out pages no one has.
 *
 * Each allocates `rounds` blocks, mostly a few bytes and now and then more
 * than a page so the memory keeps growing under all of them, fills every block
 * with its own tag and chains it to its previous one. Then it walks its chain.
 * A block two threads were both given holds the wrong tag or a broken link (no
 * lock, or a lock that does not cover the break). A block past the end of the
 * memory as this thread sees it traps on the fill (a thread that trusted a
 * size another thread's growth had already overtaken). */
static int g_gate;

__attribute__((export_name("bump_alloc_go")))
int bump_alloc_go(void)
{
    unsigned char *ahead = malloc(40 * PAGE);
    MUST(ahead);
    ahead[40 * PAGE - 1] = 1;
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify(&g_gate, ~0u);
    return 0;
}

typedef struct Block { struct Block *prev; size_t size; } Block;

__attribute__((export_name("bump_alloc_hammer")))
int bump_alloc_hammer(int tid, int rounds)
{
    unsigned char tag = (unsigned char)(tid + 1);
    Block *last = 0;
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) __builtin_wasm_memory_atomic_wait32(&g_gate, 0, -1);
    unsigned char *first = malloc(8 * PAGE);
    MUST(first);
    memset(first, tag, 8 * PAGE);
    for (int r = 0; r < rounds; r++) {
        size_t n = sizeof(Block) + 8 + (size_t)((r + tid) % 3) * 16;
        if ((r + tid * 1250) % 4999 == 0) n += PAGE + 4096;
        Block *b = malloc(n);
        MUST(b);
        memset(b, tag, n);
        b->prev = last;
        b->size = n;
        last = b;
    }
    int seen = 0;
    for (Block *b = last; b; b = b->prev, seen++) {
        MUST(seen < rounds);
        const unsigned char *body = (const unsigned char *)(b + 1);
        for (size_t i = 0; i < b->size - sizeof(Block); i++) MUST(body[i] == tag);
    }
    MUST(seen == rounds);
    for (size_t i = 0; i < 8 * PAGE; i++) MUST(first[i] == tag);
    return 0;
}
#endif
