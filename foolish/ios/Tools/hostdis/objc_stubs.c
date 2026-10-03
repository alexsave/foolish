/* objc_stubs - print "<stub address> <selector>" for every entry of a thin
 * arm64 Mach-O's __objc_stubs section.
 *
 * `otool -tV` prints a call through an objc stub as a bare `bl 0x...` in
 * binaries that are not part of the dyld shared cache (the Messages host
 * plugin MSMessageExtensionBalloonPlugin is one), so the selector a host
 * method sends is invisible. Each 32-byte stub starts with
 *   adrp x1, page ; ldr x1, [x1, #off]
 * which loads a selref; the selref points into __objc_methname. This tool
 * decodes that pair. symbolize.sh joins its output onto otool's listing.
 *
 * Build: cc -O2 -o objc_stubs objc_stubs.c
 * See docs/IMESSAGE_LIVE_ARRIVAL_HOST.md (Provenance) for how it is used.
 */
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t addr, size; uint32_t offset; int found; } Sec;

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: objc_stubs <thin arm64 mach-o>\n"); return 2; }
    size_t len = 0;
    uint8_t *b = slurp(argv[1], &len);
    if (!b || len < sizeof(struct mach_header_64)) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    const struct mach_header_64 *mh = (const void *)b;
    if (mh->magic != MH_MAGIC_64) { fprintf(stderr, "not a thin 64-bit mach-o (lipo -thin arm64 first)\n"); return 1; }
    Sec stubs = {0}, selrefs = {0}, methname = {0};
    const uint8_t *p = b + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)p;
            const struct section_64 *s = (const void *)(sg + 1);
            for (uint32_t k = 0; k < sg->nsects; k++, s++) {
                Sec *t = !strncmp(s->sectname, "__objc_stubs", 16)    ? &stubs
                       : !strncmp(s->sectname, "__objc_selrefs", 16)  ? &selrefs
                       : !strncmp(s->sectname, "__objc_methname", 16) ? &methname : NULL;
                if (t && !t->found) { t->addr = s->addr; t->size = s->size; t->offset = s->offset; t->found = 1; }
            }
        }
        p += lc->cmdsize;
    }
    if (!stubs.found) return 0; /* nothing to decode: otool already names every call */
    if (!selrefs.found || !methname.found) { fprintf(stderr, "no selrefs/methname section\n"); return 1; }
    for (uint64_t i = 0; i + 8 <= stubs.size; i += 32) {
        uint32_t w0, w1;
        memcpy(&w0, b + stubs.offset + i, 4);
        memcpy(&w1, b + stubs.offset + i + 4, 4);
        if ((w0 & 0x9f000000u) != 0x90000000u) continue; /* not adrp */
        int64_t imm = (int64_t)((((w0 >> 5) & 0x7ffffu) << 2) | ((w0 >> 29) & 3u));
        if (imm & (1 << 20)) imm -= (1 << 21);
        uint64_t pc = stubs.addr + i;
        uint64_t ref = ((pc & ~0xfffull) + (uint64_t)(imm << 12)) + (((w1 >> 10) & 0xfffu) << 3);
        if (ref < selrefs.addr || ref + 8 > selrefs.addr + selrefs.size) { printf("%llx ?\n", (unsigned long long)pc); continue; }
        uint64_t v;
        memcpy(&v, b + selrefs.offset + (ref - selrefs.addr), 8);
        uint64_t target = v & 0xffffffffull; /* chained-fixup rebase: low bits are the target */
        if (target < methname.addr || target >= methname.addr + methname.size) { printf("%llx ?\n", (unsigned long long)pc); continue; }
        const char *name = (const char *)(b + methname.offset + (target - methname.addr));
        printf("%llx %s\n", (unsigned long long)pc, name);
    }
    free(b);
    return 0;
}
