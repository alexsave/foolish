// structgen - C structs -> TypeScript accessors over wasm32 linear memory.
//
// libclang parses the headers for --target=wasm32 under ONE build's layout
// flags; every offset, size, bitfield position and array stride comes from
// clang, never from this tool. Run it once per build (layouts differ between
// builds). It writes one TS module and can print that module's layout hash,
// which the build passes to the C side (-DSG_LAYOUT_HASH=...) so a wasm module
// can prove at load time which layout it was compiled with.
//
//   structgen --cwd DIR --header H... --root T... --build NAME=FLAGS
//             [--fields T=f1,f2,SIZE]... [--const PREFIX]... [--ts OUT] [--print-hash]
//
// --fields T=...  emit (and follow) only these fields of record T; a field that
//                 does not exist is an error. SIZE requests T_SIZE.
// --const PREFIX  emit every enum constant and object-like integer #define whose
//                 name starts with PREFIX; a prefix that matches nothing is an error.
#include <clang-c/Index.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXN 64
static const char *headers[MAXN], *roots[MAXN], *prefixes[MAXN], *build, *out_ts, *cwd = ".";
static int nheaders, nroots, nprefixes, print_hash;

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("structgen: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

// ---- growable string -----------------------------------------------------
typedef struct { char *s; size_t n, cap; } Buf;
static void bprintf(Buf *b, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        size_t room = b->cap - b->n;
        int k = vsnprintf(b->s ? b->s + b->n : NULL, room, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < room) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (size_t)k + 256;
        if (!(b->s = realloc(b->s, b->cap))) die("out of memory");
    }
}
static char *xstrdup(const char *s) { char *d = strdup(s); if (!d) die("out of memory"); return d; }
static char *str(CXString cs) { char *d = xstrdup(clang_getCString(cs)); clang_disposeString(cs); return d; }
#define GROW(arr, n, cap) do { if ((n) == (cap) && !((arr) = realloc((arr), sizeof *(arr) * ((cap) = (cap) * 2 + 16)))) die("out of memory"); } while (0)

// ---- --fields requests -------------------------------------------------------
typedef struct { char *type; char *names[256]; char seen[256]; int n, used; } Spec;
static Spec specs[MAXN];
static int nspecs;
static Spec *spec_for(const char *type) {
    for (int i = 0; i < nspecs; i++) if (!strcmp(specs[i].type, type)) return &specs[i];
    return NULL;
}
static int spec_has(Spec *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->names[i], name)) return s->seen[i] = 1;
    return 0;
}

// ---- the model -------------------------------------------------------------
typedef struct {
    char *name;
    long off;            // bytes
    int lo, width;       // bitfield: first bit within byte `off`, bit count; width 0 = not a bitfield
    char kind;           // bitfield only
    int nd; long dims[8];
    int type;            // index into recs (not a bitfield)
} Field;
typedef struct {
    char *key, *name, *expr;
    long size;
    char kind;           // scalars: i u b f (records: 0)
    int record, charlike;
    CXType t;
    Field *f; int nf, capf;
} Rec;
static Rec *recs;
static int nrecs, caprecs;

static char kind_of(CXType t) {
    switch (t.kind) {
    case CXType_Bool: return 'b';
    case CXType_Float: case CXType_Double: case CXType_LongDouble: return 'f';
    case CXType_Char_U: case CXType_UChar: case CXType_UShort: case CXType_UInt: case CXType_ULong:
    case CXType_ULongLong: case CXType_UInt128: case CXType_Pointer: case CXType_BlockPointer: return 'u';
    case CXType_Enum:    // an enum is stored as its underlying integer type
        return kind_of(clang_getCanonicalType(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t))));
    default: return 'i';
    }
}

static int want(CXType t, const char *name, const char *expr) {
    char *key = str(clang_getTypeSpelling(t));
    for (int i = 0; i < nrecs; i++) if (!strcmp(recs[i].key, key)) { free(key); return i; }
    GROW(recs, nrecs, caprecs);
    Rec *r = &recs[nrecs];
    memset(r, 0, sizeof *r);
    r->key = key; r->name = xstrdup(name); r->expr = xstrdup(expr); r->t = t;
    long long sz = clang_Type_getSizeOf(t);
    if (sz < 0) die("%s: no size (incomplete type?)", name);
    r->size = (long)sz;
    r->record = t.kind == CXType_Record;
    if (!r->record) r->kind = kind_of(t);
    r->charlike = t.kind == CXType_Char_S || t.kind == CXType_Char_U;   // plain char; int8_t/uint8_t arrays are bytes
    return nrecs++;
}

static int is_ident(const char *s) {
    if (!(*s == '_' || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (; *s; s++) if (!(*s == '_' || (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    return 1;
}

typedef struct { int rec; long long base; } Walk;
static enum CXVisitorResult on_field(CXCursor c, CXClientData d) {
    Walk *w = d;
    long long bits = clang_Cursor_getOffsetOfField(c);
    if (bits < 0) die("%s: no offset for a field", recs[w->rec].name);
    bits += w->base;
    CXType ft = clang_getCanonicalType(clang_getCursorType(c));
    if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(ft))) {   // members reachable by name
        Walk inner = { w->rec, bits };
        clang_Type_visitFields(ft, on_field, &inner);
        return CXVisit_Continue;
    }
    Field f = { .name = str(clang_getCursorSpelling(c)), .off = (long)(bits / 8), .type = -1 };
    Spec *s = spec_for(recs[w->rec].name);
    if (s && !spec_has(s, f.name)) { free(f.name); return CXVisit_Continue; }
    if (clang_Cursor_isBitField(c)) {
        f.lo = (int)(bits % 8); f.width = clang_getFieldDeclBitWidth(c); f.kind = kind_of(ft);
        if (f.lo + f.width > 32) die("%s.%s: bitfields wider than a 32-bit window are not supported", recs[w->rec].name, f.name);
    } else {
        while (ft.kind == CXType_ConstantArray) {
            if (f.nd == 8) die("%s.%s: too many array dimensions", recs[w->rec].name, f.name);
            f.dims[f.nd++] = (long)clang_getArraySize(ft);
            ft = clang_getCanonicalType(clang_getArrayElementType(ft));
        }
        if (ft.kind == CXType_IncompleteArray || ft.kind == CXType_VariableArray)
            die("%s.%s: arrays without a constant size are not supported", recs[w->rec].name, f.name);
        char *sp = str(clang_getTypeSpelling(ft));
        const char *id = sp;
        if (!strncmp(id, "struct ", 7)) id += 7; else if (!strncmp(id, "union ", 6)) id += 6; else if (!strncmp(id, "enum ", 5)) id += 5;
        Buf name = {0}, expr = {0};
        if (is_ident(id)) bprintf(&name, "%s", id); else bprintf(&name, "%s_%s", recs[w->rec].name, f.name);
        bprintf(&expr, "(%s).%s", recs[w->rec].expr, f.name);
        for (int i = 0; i < f.nd; i++) bprintf(&expr, "[0]");
        f.type = want(ft, name.s, expr.s);
        free(sp); free(name.s); free(expr.s);
    }
    Rec *r = &recs[w->rec];
    GROW(r->f, r->nf, r->capf);
    r->f[r->nf++] = f;
    return CXVisit_Continue;
}

// ---- constants ----------------------------------------------------------------
typedef struct { char *name; int prefix; long long value; int macro; } Const;
static Const *consts;
static int nconsts, capconsts;
static int prefix_of(const char *name) {
    for (int i = 0; i < nprefixes; i++) if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return i;
    return -1;
}
static void add_const(const char *name, int macro, long long value) {
    int p = prefix_of(name);
    if (p < 0) return;
    for (int i = 0; i < nconsts; i++) if (!strcmp(consts[i].name, name)) return;
    GROW(consts, nconsts, capconsts);
    consts[nconsts++] = (Const){ xstrdup(name), p, value, macro };
}
static enum CXChildVisitResult on_macro(CXCursor c, CXCursor parent, CXClientData d) {
    (void)parent; (void)d;
    if (clang_getCursorKind(c) == CXCursor_MacroDefinition && !clang_Cursor_isMacroFunctionLike(c) && !clang_Cursor_isMacroBuiltin(c)) {
        char *name = str(clang_getCursorSpelling(c));
        add_const(name, 1, 0);
        free(name);
    }
    return CXChildVisit_Continue;
}
static enum CXChildVisitResult on_decl(CXCursor c, CXCursor parent, CXClientData d) {
    (void)parent; (void)d;
    enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_EnumConstantDecl) {
        char *name = str(clang_getCursorSpelling(c));
        int idx;
        if (sscanf(name, "__structgen_m%d", &idx) == 1) consts[idx].value = clang_getEnumConstantDeclValue(c);
        else add_const(name, 0, clang_getEnumConstantDeclValue(c));
        free(name);
    }
    if (k == CXCursor_EnumDecl || k == CXCursor_TypedefDecl || k == CXCursor_StructDecl || k == CXCursor_UnionDecl)
        return CXChildVisit_Recurse;
    if (k == CXCursor_VarDecl) {
        char *name = str(clang_getCursorSpelling(c));
        int i;
        if (sscanf(name, "__structgen_root%d", &i) == 1) {
            CXType t = clang_getCanonicalType(clang_getPointeeType(clang_getCursorType(c)));
            Buf expr = {0};
            bprintf(&expr, "*(%s *)0", roots[i]);
            int r = want(t, roots[i], expr.s);
            if (!recs[r].record) die("root %s is not a struct or union", roots[i]);
            free(expr.s);
        }
        free(name);
    }
    return CXChildVisit_Continue;
}

// ---- TU ----------------------------------------------------------------------
static const char *args[512];
static int nargs;
static void build_args(const char *flags) {
    args[nargs++] = "--target=wasm32"; args[nargs++] = "-ffreestanding"; args[nargs++] = "-iquote"; args[nargs++] = ".";
    // libclang does not find its own builtin headers (stdint.h, stdbool.h):
    // use the resource dir of the clang this tool was built against.
    args[nargs++] = "-resource-dir"; args[nargs++] = SG_RESOURCE_DIR;
    static const char *with_arg[] = { "-isystem", "-include", "-I", "-D", "-U", "-iquote", "-idirafter" };
    static const char *keep[] = { "-D", "-U", "-I", "-isystem", "-iquote", "-idirafter", "-include", "-std=",
                                  "-fsigned-char", "-funsigned-char", "-fshort-enums", "-fno-short-enums", "-fpack-struct", "-m" };
    char *fl = xstrdup(flags);   // lives for the process: args[] points into it
    for (char *tok = strtok(fl, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
        if (nargs > 500) die("too many build flags");
        int pair = 0;
        for (size_t k = 0; k < sizeof with_arg / sizeof *with_arg; k++) pair |= !strcmp(tok, with_arg[k]);
        if (pair) {
            args[nargs++] = tok;
            if (!(tok = strtok(NULL, " \t\n"))) die("build flag %s needs a value", args[nargs - 1]);
            args[nargs++] = tok;
            continue;
        }
        for (size_t k = 0; k < sizeof keep / sizeof *keep; k++)
            if (!strncmp(tok, keep[k], strlen(keep[k]))) { args[nargs++] = tok; break; }
    }
}
static CXTranslationUnit parse(CXIndex idx, const char *src, unsigned opts) {
    struct CXUnsavedFile probe = { "__structgen_probe.c", src, (unsigned long)strlen(src) };
    CXTranslationUnit tu;
    enum CXErrorCode e = clang_parseTranslationUnit2(idx, "__structgen_probe.c", args, nargs, &probe, 1,
        opts | CXTranslationUnit_SkipFunctionBodies, &tu);
    if (e != CXError_Success) die("libclang parse failed (CXErrorCode %d)", (int)e);
    int errors = 0;
    for (unsigned i = 0; i < clang_getNumDiagnostics(tu); i++) {
        CXDiagnostic dg = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(dg) >= CXDiagnostic_Error) {
            char *m = str(clang_formatDiagnostic(dg, clang_defaultDiagnosticDisplayOptions()));
            fprintf(stderr, "%s\n", m); free(m); errors++;
        }
        clang_disposeDiagnostic(dg);
    }
    if (errors) die("%d compile error(s)%s", errors, nconsts ? " (a --const prefix matching a non-integer #define shows as __structgen_m*)" : "");
    return tu;
}

// ---- emission ------------------------------------------------------------------
static const char *scalar_access(char kind, long size, int write, const char **vt) {
    static const struct { char k; long sz; const char *rd, *wr, *vt; } T[] = {
        { 'i', 1, "m.i8[%s]", "m.i8[%s] = v", "number" }, { 'u', 1, "m.u8[%s]", "m.u8[%s] = v", "number" },
        { 'b', 1, "m.u8[%s] !== 0", "m.u8[%s] = v ? 1 : 0", "boolean" },
        { 'i', 2, "m.dv.getInt16(%s, true)", "m.dv.setInt16(%s, v, true)", "number" },
        { 'u', 2, "m.dv.getUint16(%s, true)", "m.dv.setUint16(%s, v, true)", "number" },
        { 'i', 4, "m.dv.getInt32(%s, true)", "m.dv.setInt32(%s, v, true)", "number" },
        { 'u', 4, "m.dv.getUint32(%s, true)", "m.dv.setUint32(%s, v, true)", "number" },
        { 'f', 4, "m.dv.getFloat32(%s, true)", "m.dv.setFloat32(%s, v, true)", "number" },
        { 'f', 8, "m.dv.getFloat64(%s, true)", "m.dv.setFloat64(%s, v, true)", "number" },
        { 'i', 8, "m.dv.getBigInt64(%s, true)", "m.dv.setBigInt64(%s, v, true)", "bigint" },
        { 'u', 8, "m.dv.getBigUint64(%s, true)", "m.dv.setBigUint64(%s, v, true)", "bigint" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].k == kind && T[i].sz == size) { *vt = T[i].vt; return write ? T[i].wr : T[i].rd; }
    return NULL;
}
// "p" or "p + off": the address of a member at byte offset `off`.
static const char *at(long off) {
    static char buf[8][32];
    static int k;
    char *b = buf[k++ % 8];
    if (off) snprintf(b, sizeof buf[0], "p + %ld", off); else snprintf(b, sizeof buf[0], "p");
    return b;
}
// A window of `bytes` little-endian bytes at p + off, as an unsigned integer.
static void window(char *out, size_t cap, long off, int bytes) {
    if (bytes == 1) snprintf(out, cap, "m.u8[%s]", at(off));
    else snprintf(out, cap, "m.dv.getUint%d(%s, true)", bytes == 2 ? 16 : 32, at(off));
}

static void emit_record(Buf *ts, Rec *r, int *strings) {
    Spec *s = spec_for(r->name);
    bprintf(ts, "// %s\n", r->name);
    if (!s || spec_has(s, "SIZE")) bprintf(ts, "export const %s_SIZE = %ld;\n", r->name, r->size);
    int all_bits = r->nf > 0 && !s;
    for (int j = 0; j < r->nf; j++) all_bits &= r->f[j].width > 0;
    if (r->size == 1 || r->size == 2 || r->size == 4) {   // the whole record as one unsigned integer
        const char *vt, *rd = scalar_access('u', r->size, 0, &vt), *wr = scalar_access('u', r->size, 1, &vt);
        bprintf(ts, "export const %s_raw_get = (m: Mem, p: number) => ", r->name); bprintf(ts, rd, "p");
        bprintf(ts, ";\nexport const %s_raw_set = (m: Mem, p: number, v: number) => { ", r->name); bprintf(ts, wr, "p");
        bprintf(ts, "; };\n");
    }
    if (all_bits && r->size <= 4) {                         // pack/unpack the raw integer by field
        bprintf(ts, "export const %s_pack = (", r->name);
        for (int j = 0; j < r->nf; j++) bprintf(ts, "%s_%s: %s", j ? ", " : "", r->f[j].name, r->f[j].kind == 'b' ? "boolean" : "number");
        bprintf(ts, ") => (");
        for (int j = 0; j < r->nf; j++) {
            Field *f = &r->f[j];
            int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "%s((%s_%s << %d) & %u)", j ? " | " : "", f->kind == 'b' ? "+" : "", f->name, pos,
                (unsigned)((((1ull << f->width) - 1) << pos) & 0xffffffffull));
        }
        bprintf(ts, ")%s;\n", r->size == 4 ? " >>> 0" : "");
        for (int j = 0; j < r->nf; j++) {
            Field *f = &r->f[j];
            int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "export const %s_unpack_%s = (r: number) => %s(r << %d) %s %d%s;\n", r->name, f->name,
                f->kind == 'b' ? "(" : "", 32 - pos - f->width, f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
        }
    }
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width) {
            int bytes = (f->lo + f->width + 7) / 8;
            if (bytes == 3) bytes = 4;
            char W[64];
            window(W, sizeof W, f->off, bytes);
            unsigned mask = (unsigned)((((1ull << f->width) - 1) << f->lo) & 0xffffffffull);
            bprintf(ts, "export const %s_get_%s = (m: Mem, p: number) => %s(%s << %d) %s %d%s;\n", r->name, f->name,
                f->kind == 'b' ? "(" : "", W, 32 - f->lo - f->width, f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            const char *vt = f->kind == 'b' ? "boolean" : "number", *v = f->kind == 'b' ? "+v" : "v";
            if (bytes == 1)
                bprintf(ts, "export const %s_set_%s = (m: Mem, p: number, v: %s) => { m.u8[%s] = (%s & %d) | ((%s << %d) & %u); };\n",
                    r->name, f->name, vt, at(f->off), W, (int)~mask, v, f->lo, mask);
            else
                bprintf(ts, "export const %s_set_%s = (m: Mem, p: number, v: %s) => { m.dv.setUint%d(%s, ((%s & %d) | ((%s << %d) & %u)) >>> 0, true); };\n",
                    r->name, f->name, vt, bytes * 8, at(f->off), W, (int)~mask, v, f->lo, mask);
            continue;
        }
        Rec *t = &recs[f->type];
        Buf addr = {0}, idxs = {0};
        bprintf(&addr, "%s", at(f->off));
        long stride = t->size;
        for (int k = f->nd - 1; k >= 0; k--) {
            if (stride == 1) bprintf(&addr, " + i%d", k); else bprintf(&addr, " + i%d * %ld", k, stride);
            stride *= f->dims[k];
        }
        bprintf(&idxs, "%s", "");
        for (int k = 0; k < f->nd; k++) bprintf(&idxs, ", i%d: number", k);
        bprintf(ts, "export const %s_%s_at = (p: number%s) => %s;\n", r->name, f->name, idxs.s, addr.s);
        for (int k = 0; k < f->nd; k++) {
            if (k) bprintf(ts, "export const %s_%s_LEN%d = %ld;\n", r->name, f->name, k, f->dims[k]);
            else bprintf(ts, "export const %s_%s_LEN = %ld;\n", r->name, f->name, f->dims[k]);
        }
        const char *vt, *rd = t->record ? NULL : scalar_access(t->kind, t->size, 0, &vt);
        if (rd) {
            bprintf(ts, "export const %s_get_%s = (m: Mem, p: number%s) => ", r->name, f->name, idxs.s); bprintf(ts, rd, addr.s);
            bprintf(ts, ";\nexport const %s_set_%s = (m: Mem, p: number%s, v: %s) => { ", r->name, f->name, idxs.s, vt);
            bprintf(ts, scalar_access(t->kind, t->size, 1, &vt), addr.s);
            bprintf(ts, "; };\n");
        }
        if (t->charlike && f->nd == 1) {   // char[N]: a NUL-terminated UTF-8 string of at most N-1 bytes
            *strings = 1;
            bprintf(ts, "export const %s_get_%s_str = (m: Mem, p: number) => cstrGet(m, %s, %ld);\n", r->name, f->name, at(f->off), f->dims[0]);
            bprintf(ts, "export const %s_set_%s_str = (m: Mem, p: number, v: string) => { cstrSet(m, %s, %ld, v, '%s.%s'); };\n",
                r->name, f->name, at(f->off), f->dims[0], r->name, f->name);
        }
        free(addr.s); free(idxs.s);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void check_unique_exports(const Buf *ts) {   // generated names are joined with '_' and could collide
    char **names = NULL; int n = 0, cap = 0;
    for (const char *p = ts->s; (p = strstr(p, "export const ")); ) {
        p += 13;
        size_t len = strcspn(p, " =");
        GROW(names, n, cap);
        names[n] = malloc(len + 1); memcpy(names[n], p, len); names[n++][len] = 0;
    }
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    for (int i = 1; i < n; i++) if (!strcmp(names[i - 1], names[i])) die("generated name %s is emitted twice", names[i]);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void usage(void) {
    fputs("usage: structgen --cwd DIR --header H... --root T... --build NAME=FLAGS\n"
          "                 [--fields T=f1,f2,SIZE]... [--const PREFIX]... [--ts OUT.ts] [--print-hash]\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--print-hash")) { print_hash = 1; continue; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) usage();
        if (strncmp(a, "--", 2)) die("unexpected argument %s", a);
        if (i + 1 >= argc || !strncmp(argv[i + 1], "--", 2)) die("%s needs a value", a);
        const char *v = argv[++i];
        if (!strcmp(a, "--header")) { if (nheaders == MAXN) die("too many --header"); headers[nheaders++] = v; }
        else if (!strcmp(a, "--root")) { if (nroots == MAXN) die("too many --root"); roots[nroots++] = v; }
        else if (!strcmp(a, "--const")) { if (nprefixes == MAXN) die("too many --const"); prefixes[nprefixes++] = v; }
        else if (!strcmp(a, "--build")) { if (build) die("one --build per run (run once per build)"); build = v; }
        else if (!strcmp(a, "--ts")) out_ts = v;
        else if (!strcmp(a, "--cwd")) cwd = v;
        else if (!strcmp(a, "--fields")) {
            if (nspecs == MAXN) die("too many --fields");
            Spec *s = &specs[nspecs++];
            char *eq = strchr(s->type = xstrdup(v), '=');
            if (!eq || eq == s->type || !eq[1]) die("--fields must be TYPE=f1,f2,...");
            *eq = 0;
            for (char *tok = strtok(eq + 1, ","); tok; tok = strtok(NULL, ",")) {
                if (s->n == 256) die("too many fields for %s", s->type);
                s->names[s->n++] = tok;
            }
        }
        else die("unknown argument %s", a);
    }
    if (!nheaders || !nroots || !build) usage();
    if (!out_ts && !print_hash) die("nothing to do: give --ts and/or --print-hash");
    const char *eq = strchr(build, '=');
    if (!eq || eq == build) die("--build must be NAME=FLAGS");
    static char out_abs[4096];   // --ts is relative to where we were started, not to --cwd
    if (out_ts && out_ts[0] != '/') {
        if (!getcwd(out_abs, sizeof out_abs) || strlen(out_abs) + strlen(out_ts) + 2 > sizeof out_abs) die("path too long");
        strcat(strcat(out_abs, "/"), out_ts);
        out_ts = out_abs;
    }
    if (chdir(cwd)) die("cannot cd to %s", cwd);
    build_args(eq + 1);

    Buf src = {0};
    for (int i = 0; i < nheaders; i++) bprintf(&src, "#include \"%s\"\n", headers[i]);
    for (int i = 0; i < nroots; i++) bprintf(&src, "%s *__structgen_root%d;\n", roots[i], i);
    CXIndex idx = clang_createIndex(0, 0);
    CXTranslationUnit tu = parse(idx, src.s, nprefixes ? CXTranslationUnit_DetailedPreprocessingRecord : 0);
    if (nprefixes) {
        // #defines have no value in the AST: collect their names, then let clang
        // evaluate each one as an enumerator in a second parse.
        clang_visitChildren(clang_getTranslationUnitCursor(tu), on_macro, NULL);
        if (nconsts) {
            for (int i = 0; i < nconsts; i++) bprintf(&src, "enum { __structgen_m%d = (%s) };\n", i, consts[i].name);
            clang_disposeTranslationUnit(tu);
            tu = parse(idx, src.s, 0);
        }
    }
    clang_visitChildren(clang_getTranslationUnitCursor(tu), on_decl, NULL);
    for (int i = 0; i < nprefixes; i++) {
        int hit = 0;
        for (int j = 0; j < nconsts; j++) hit |= consts[j].prefix == i;
        if (!hit) die("--const %s matches no enum constant or #define", prefixes[i]);
    }
    for (int i = 0; i < nrecs; i++) if (recs[i].record) {
        Walk w = { i, 0 };
        clang_Type_visitFields(recs[i].t, on_field, &w);
    }
    for (int i = 0; i < nspecs; i++) {
        Spec *s = &specs[i];
        int found = 0;
        for (int j = 0; j < nrecs; j++) found |= recs[j].record && !strcmp(recs[j].name, s->type);
        if (!found) die("--fields %s: no such record reached from the roots", s->type);
        spec_has(s, "SIZE");
        for (int j = 0; j < s->n; j++) if (!s->seen[j]) die("--fields %s: no field named %s", s->type, s->names[j]);
    }

    // ---- the hashed body: every fact the TS relies on ------------------------
    Buf ts = {0};
    int strings = 0;
    if (nconsts) bprintf(&ts, "// constants\n");
    for (int p = 0; p < nprefixes; p++)
        for (int m = 1; m >= 0; m--)
            for (int j = 0; j < nconsts; j++)
                if (consts[j].prefix == p && consts[j].macro == m) bprintf(&ts, "export const %s = %lld;\n", consts[j].name, consts[j].value);
    for (int i = 0; i < nrecs; i++) if (recs[i].record) emit_record(&ts, &recs[i], &strings);
    check_unique_exports(&ts);
    unsigned hash = 2166136261u;   // FNV-1a
    for (size_t i = 0; i < ts.n; i++) hash = (hash ^ (unsigned char)ts.s[i]) * 16777619u;

    if (out_ts) {
        FILE *fp = fopen(out_ts, "w");
        if (!fp) die("cannot write %s", out_ts);
        fprintf(fp, "// GENERATED by tools/structgen - do not edit.\n// build: %.*s; roots: ", (int)(eq - build), build);
        for (int i = 0; i < nroots; i++) fprintf(fp, "%s%s", i ? ", " : "", roots[i]);
        fprintf(fp, "\nexport const LAYOUT_HASH = 0x%08x;\n"
                    "export interface Mem { u8: Uint8Array; i8: Int8Array; dv: DataView }\n"
                    "export const memOf = (b: ArrayBuffer): Mem => ({ u8: new Uint8Array(b), i8: new Int8Array(b), dv: new DataView(b) });\n", hash);
        if (strings) fputs(   // char[N] as UTF-8 + NUL; ASCII stays off TextEncoder/TextDecoder
            "const utf8Enc = /* @__PURE__ */ new TextEncoder(), utf8Dec = /* @__PURE__ */ new TextDecoder();\n"
            "const cstrGet = (m: Mem, a: number, n: number) => {\n"
            "    let s = '', e = a;\n"
            "    for (const end = a + n; e < end; e++) { const c = m.u8[e]; if (c === 0) return s; if (c > 127) break; s += String.fromCharCode(c); }\n"
            "    while (e < a + n && m.u8[e] !== 0) e++;\n"
            "    return e === a + n && s.length === n ? s : utf8Dec.decode(m.u8.subarray(a, e));\n"
            "};\n"
            "const cstrSet = (m: Mem, a: number, n: number, s: string, what: string) => {\n"
            "    let i = 0;\n"
            "    const len = s.length;\n"
            "    if (len < n) while (i < len) { const c = s.charCodeAt(i); if (c === 0 || c > 127) break; i++; }\n"
            "    if (i === len && len < n) { for (let j = 0; j < len; j++) m.u8[a + j] = s.charCodeAt(j); }\n"
            "    else {\n"
            "        const b = utf8Enc.encode(s);\n"
            "        if (b.length > n - 1) throw new RangeError(`${what}: ${b.length} UTF-8 bytes do not fit in ${n - 1}`);\n"
            "        if (b.includes(0)) throw new RangeError(`${what}: contains a NUL`);\n"
            "        m.u8.set(b, a); i = b.length;\n"
            "    }\n"
            "    for (let j = i; j < n; j++) m.u8[a + j] = 0;\n"
            "};\n", fp);
        fwrite(ts.s, 1, ts.n, fp);
        if (fclose(fp)) die("cannot write %s", out_ts);
    }
    if (print_hash) printf("0x%08x\n", hash);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
    return 0;
}
