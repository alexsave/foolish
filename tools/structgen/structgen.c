// structgen - C structs -> TypeScript accessors over wasm32 linear memory.
//
// libclang parses the headers for --target=wasm32 under ONE build's layout
// flags; every offset, size, bitfield position and array stride comes from
// clang, never from this tool. Run it once per build (layouts differ between
// builds). It writes one TS module of accessors and computes that layout's
// hash, which the build passes to the C side (-DSG_LAYOUT_HASH=...) so a wasm
// module can prove at load time which layout it was compiled with.
//
//   structgen --cwd DIR --header H... --root T... --build NAME=FLAGS
//             [--fields T=f1,f2,SIZE]... [--const PREFIX]...
//             [--snapshot T]... [--count T.f=c]... [--snapshot-only]
//             [--ts OUT] [--hash-ts OUT] [--print-hash]
//
// --ts OUT        the accessors (no hash: a host that only checks the hash must
//                 not have to import the accessors, which read the raw struct)
// --hash-ts OUT   a module holding only `export const LAYOUT_HASH = 0x...;`
// --print-hash    the same hash on stdout
//
// --fields T=...  emit (and follow) only these fields of record T; a field that
//                 does not exist is an error. SIZE requests T_SIZE.
// --const PREFIX  emit every enum constant and object-like integer #define whose
//                 name starts with PREFIX; a prefix that matches nothing is an error.
//
// --snapshot T    emit `interface T_Snap` and `readT(m, p): T_Snap`, which copies the
//                 record out of wasm memory into a plain object that holds no
//                 pointer into it (so it outlives the memory's next write), and the
//                 same for every record T reaches. Fields are named in camelCase;
//                 nested records become objects, arrays become arrays, char[N]
//                 becomes a string. The types are readonly: a snapshot is a value.
// --count T.f=c   in a snapshot, array T.f holds only its first T.c elements (a
//                 char[N] is then c bytes of UTF-8 rather than NUL-terminated). The
//                 count field itself is not copied (it is the array's length), and
//                 a count outside 0..N throws a RangeError rather than reading past
//                 the array.
// --snapshot-only the module holds the snapshot readers and the constants, not the
//                 per-field accessors.
#include <clang-c/Index.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXN 64
static const char *headers[MAXN], *roots[MAXN], *prefixes[MAXN], *snaps[MAXN], *build, *out_ts, *out_hash_ts, *cwd = ".";
static int nheaders, nroots, nprefixes, nsnaps, print_hash, snapshot_only;
typedef struct { char *type, *field, *count; int used; } Count;
static Count counts[MAXN];
static int ncounts;

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
    int record, charlike, is_union;
    int snap;            // --snapshot: 1 once reached from a snapshot root
    CXType t;
    Field *f; int nf, capf;
} Rec;
static Rec *recs;
static int nrecs, caprecs;
static int root_rec[MAXN];   // --root i -> its record

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

// `key` is the canonical type's spelling, used ONLY to recognise the same type
// reached twice within this run. It is never emitted and never hashed: libclang
// renders a canonical type differently across LLVM versions.
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
    r->is_union = r->record && clang_getCursorKind(clang_getTypeDeclaration(t)) == CXCursor_UnionDecl;
    if (!r->record) r->kind = kind_of(t);
    r->charlike = t.kind == CXType_Char_S || t.kind == CXType_Char_U;   // plain char; int8_t/uint8_t arrays are bytes
    return nrecs++;
}

static int is_ident(const char *s) {
    if (!(*s == '_' || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (; *s; s++) if (!(*s == '_' || (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    return 1;
}

// The name the HEADER gives a field's element type: the typedef or struct/union
// tag exactly as declared, or NULL for an unnamed record. This, not a rendering
// of the type, names the generated accessors (Card_get_suit): declared names are
// what the header author wrote, so every libclang reports them alike.
// `t` is the field's declared (sugared) type with its `nd` array levels still on.
static char *declared_name(CXType t, int nd) {
    for (;;) {
        if (t.kind == CXType_Elaborated) { t = clang_Type_getNamedType(t); continue; }
        if (nd > 0) {   // an array level, possibly behind a typedef of the array type
            if (t.kind == CXType_Typedef) { t = clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(t)); continue; }
            if (t.kind != CXType_ConstantArray) return NULL;
            t = clang_getArrayElementType(t); nd--;
            continue;
        }
        if (t.kind == CXType_Typedef) return str(clang_getCursorSpelling(clang_getTypeDeclaration(t)));
        if (t.kind == CXType_Record || t.kind == CXType_Enum) {
            CXCursor d = clang_getTypeDeclaration(t);
            return clang_Cursor_isAnonymous(d) ? NULL : str(clang_getCursorSpelling(d));
        }
        return NULL;
    }
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
        char *id = declared_name(clang_getCursorType(c), f.nd);
        Buf name = {0}, expr = {0};
        if (id && is_ident(id)) bprintf(&name, "%s", id); else bprintf(&name, "%s_%s", recs[w->rec].name, f.name);
        bprintf(&expr, "(%s).%s", recs[w->rec].expr, f.name);
        for (int i = 0; i < f.nd; i++) bprintf(&expr, "[0]");
        f.type = want(ft, name.s, expr.s);
        free(id); free(name.s); free(expr.s);
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
            root_rec[i] = r;
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

// Which string helpers the module uses.
#define STR_ACCESSORS 1   // cstrGet and cstrSet (char[N] accessors)
#define STR_CSTR_GET  2   // cstrGet (a snapshot's NUL-terminated char[N])
#define STR_UTF8_GET  4   // utf8Get (a snapshot's counted char[N])

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
            *strings |= STR_ACCESSORS;
            bprintf(ts, "export const %s_get_%s_str = (m: Mem, p: number) => cstrGet(m, %s, %ld);\n", r->name, f->name, at(f->off), f->dims[0]);
            bprintf(ts, "export const %s_set_%s_str = (m: Mem, p: number, v: string) => { cstrSet(m, %s, %ld, v, '%s.%s'); };\n",
                r->name, f->name, at(f->off), f->dims[0], r->name, f->name);
        }
        free(addr.s); free(idxs.s);
    }
}

// ---- snapshots ---------------------------------------------------------------------

// snake_case -> camelCase, into a static buffer.
static const char *camel(const char *s) {
    static char buf[8][256];
    static int k;
    char *b = buf[k++ % 8];
    size_t w = 0;
    for (int up = 0; *s && w + 1 < sizeof buf[0]; s++) {
        if (*s == '_' && w > 0) { up = 1; continue; }
        b[w++] = (char)(up && *s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        up = 0;
    }
    b[w] = 0;
    return b;
}

static Count *count_for(const char *rec, const char *field) {
    for (int i = 0; i < ncounts; i++)
        if (!strcmp(counts[i].type, rec) && !strcmp(counts[i].field, field)) return &counts[i];
    return NULL;
}
static int is_count_field(const char *rec, const char *field) {
    for (int i = 0; i < ncounts; i++)
        if (!strcmp(counts[i].type, rec) && !strcmp(counts[i].count, field)) return 1;
    return 0;
}
static Field *field_named(Rec *r, const char *name) {
    for (int j = 0; j < r->nf; j++) if (!strcmp(r->f[j].name, name)) return &r->f[j];
    return NULL;
}

// Marks `ri` and every record its fields reach as snapshotted.
static void snap_mark(int ri) {
    Rec *r = &recs[ri];
    if (r->snap) return;
    if (r->is_union) die("--snapshot: %s is a union, which has no one value to copy", r->name);
    r->snap = 1;
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        if (f->nd > 1) die("--snapshot: %s.%s has %d array dimensions; a snapshot copies one", r->name, f->name, f->nd);
        if (t->record) snap_mark(f->type);
    }
}

// The TS type of a field's element in a snapshot.
static const char *snap_elem_type(Rec *t) {
    static char buf[4][128];
    static int k;
    char *b = buf[k++ % 4];
    const char *vt = "number";
    if (t->record) snprintf(b, sizeof buf[0], "%s_Snap", t->name);
    else { scalar_access(t->kind, t->size, 0, &vt); snprintf(b, sizeof buf[0], "%s", vt); }
    return b;
}

static void emit_snapshot(Buf *ts, Rec *r, int *strings) {
    bprintf(ts, "// %s snapshot\nexport interface %s_Snap {", r->name, r->name);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        const char *type;
        if (f->width) type = f->kind == 'b' ? "boolean" : "number";
        else {
            Rec *t = &recs[f->type];
            if (t->charlike && f->nd == 1) type = "string";
            else if (f->nd == 1) { static char arr[160]; snprintf(arr, sizeof arr, "readonly %s[]", snap_elem_type(t)); type = arr; }
            else type = snap_elem_type(t);
        }
        bprintf(ts, " readonly %s: %s;", camel(f->name), type);
    }
    bprintf(ts, " }\nexport const read%s = (m: Mem, p: number): %s_Snap => {\n", r->name, r->name);
    // Counts first: every one is checked before anything is read with it.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        Count *c = f->width ? NULL : count_for(r->name, f->name);
        if (!c) continue;
        Field *cf = field_named(r, c->count);
        const char *vt, *rd = scalar_access(recs[cf->type].kind, recs[cf->type].size, 0, &vt);
        bprintf(ts, "    const n_%s = ", f->name);
        bprintf(ts, rd, at(cf->off));
        bprintf(ts, ";\n    if (!(n_%s >= 0 && n_%s <= %ld)) throw new RangeError(`%s.%s: count ${n_%s} is outside 0..%ld`);\n",
                f->name, f->name, f->dims[0], r->name, f->name, f->name, f->dims[0]);
    }
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || f->nd != 1 || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        if (t->charlike) continue;
        char n[160];
        if (count_for(r->name, f->name)) snprintf(n, sizeof n, "n_%s", f->name); else snprintf(n, sizeof n, "%ld", f->dims[0]);
        bprintf(ts, "    const %s: %s[] = new Array(%s);\n", camel(f->name), snap_elem_type(t), n);
        bprintf(ts, "    for (let i = 0; i < %s; i++) %s[i] = ", n, camel(f->name));
        char a[96];
        if (t->size == 1) snprintf(a, sizeof a, "%s + i", at(f->off)); else snprintf(a, sizeof a, "%s + i * %ld", at(f->off), t->size);
        if (t->record) bprintf(ts, "read%s(m, %s);\n", t->name, a);
        else { const char *vt; bprintf(ts, scalar_access(t->kind, t->size, 0, &vt), a); bprintf(ts, ";\n"); }
    }
    // A record of bitfields that fits one integer is read once and unpacked.
    int packed = r->nf > 0 && (r->size == 1 || r->size == 2 || r->size == 4);
    for (int j = 0; j < r->nf; j++) packed &= r->f[j].width > 0;
    if (packed) {
        const char *vt;
        bprintf(ts, "    const raw = ");
        bprintf(ts, scalar_access('u', r->size, 0, &vt), "p");
        bprintf(ts, ";\n");
    }
    bprintf(ts, "    return {");
    for (int j = 0, first = 1; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(ts, "%s%s: ", first ? " " : ", ", camel(f->name));
        first = 0;
        if (packed) {
            const int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "%s(raw << %d) %s %d%s", f->kind == 'b' ? "(" : "", 32 - pos - f->width,
                    f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            continue;
        }
        if (f->width) {
            int bytes = (f->lo + f->width + 7) / 8;
            if (bytes == 3) bytes = 4;
            char W[64];
            window(W, sizeof W, f->off, bytes);
            bprintf(ts, "%s(%s << %d) %s %d%s", f->kind == 'b' ? "(" : "", W, 32 - f->lo - f->width,
                    f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            continue;
        }
        Rec *t = &recs[f->type];
        if (t->charlike && f->nd == 1) {
            if (count_for(r->name, f->name)) { *strings |= STR_UTF8_GET; bprintf(ts, "utf8Get(m, %s, n_%s)", at(f->off), f->name); }
            else { *strings |= STR_CSTR_GET; bprintf(ts, "cstrGet(m, %s, %ld)", at(f->off), f->dims[0]); }
        } else if (f->nd == 1) {
            bprintf(ts, "%s", camel(f->name));
        } else if (t->record) {
            bprintf(ts, "read%s(m, %s)", t->name, at(f->off));
        } else {
            const char *vt;
            bprintf(ts, scalar_access(t->kind, t->size, 0, &vt), at(f->off));
        }
    }
    bprintf(ts, " };\n};\n");
}

// ---- the layout hash -------------------------------------------------------------
// FNV-1a over the LAYOUT facts the emitted TS relies on, and nothing else:
//   every emitted field, walked from each --root along its field path
//   ("Game.players.hand.suit"): byte offset, bit range and bit kind, or array
//   dims, element size, element kind (i u b f, r = record) and char-ness;
//   each record SIZE the module emits (requested, or its raw_get/raw_set);
//   each --const constant's name and value, in emission order.
// Paths are made of --root names (the caller's) and field names (the header's);
// record and typedef names are NOT in it. Renaming a typedef renames the
// generated accessors (a TS compile error at every user) but leaves the hash
// alone, and libclang's rendering of a type, which differs between LLVM
// versions, can never reach it. tools/structgen/test/hash.sh pins both halves.
static unsigned layout_hash = 2166136261u;
static void hput(const char *fmt, ...) {
    char line[1024];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof line) die("hash line too long");
    for (int i = 0; i <= k; i++) layout_hash = (layout_hash ^ (unsigned char)(i < k ? line[i] : '\n')) * 16777619u;
}
static void hash_record(int ri, const char *path) {
    Rec *r = &recs[ri];
    Spec *s = spec_for(r->name);
    if (!s || spec_has(s, "SIZE") || r->size == 1 || r->size == 2 || r->size == 4) hput("%s size %ld", path, r->size);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width) { hput("%s.%s off %ld bits %d+%d %c", path, f->name, f->off, f->lo, f->width, f->kind); continue; }
        Rec *t = &recs[f->type];
        char dims[128] = "";
        for (int k = 0; k < f->nd; k++) snprintf(dims + strlen(dims), sizeof dims - strlen(dims), "[%ld]", f->dims[k]);
        hput("%s.%s off %ld%s elem %ld %c%s", path, f->name, f->off, dims, t->size, t->record ? 'r' : t->kind, t->charlike ? " char" : "");
        if (t->record) {
            Buf child = {0};
            bprintf(&child, "%s.%s", path, f->name);
            hash_record(f->type, child.s);
            free(child.s);
        }
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

static const char *absolute(const char *path) {
    if (!path || path[0] == '/') return path;
    char dir[4096];
    if (!getcwd(dir, sizeof dir)) die("cannot read the working directory");
    char *abs = malloc(strlen(dir) + strlen(path) + 2);
    if (!abs) die("out of memory");
    return strcat(strcat(strcpy(abs, dir), "/"), path);
}

static const char *build_name_end;   // build .. build_name_end is NAME in --build NAME=FLAGS

// The header both generated modules start with.
static void emit_banner(FILE *fp) {
    fprintf(fp, "// GENERATED by tools/structgen - do not edit.\n// build: %.*s; roots: ", (int)(build_name_end - build), build);
    for (int i = 0; i < nroots; i++) fprintf(fp, "%s%s", i ? ", " : "", roots[i]);
    fputc('\n', fp);
}

static void usage(void) {
    fputs("usage: structgen --cwd DIR --header H... --root T... --build NAME=FLAGS\n"
          "                 [--fields T=f1,f2,SIZE]... [--const PREFIX]...\n"
          "                 [--snapshot T]... [--count T.f=c]... [--snapshot-only]\n"
          "                 [--ts OUT.ts] [--hash-ts OUT.ts] [--print-hash]\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--print-hash")) { print_hash = 1; continue; }
        if (!strcmp(a, "--snapshot-only")) { snapshot_only = 1; continue; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) usage();
        if (strncmp(a, "--", 2)) die("unexpected argument %s", a);
        if (i + 1 >= argc || !strncmp(argv[i + 1], "--", 2)) die("%s needs a value", a);
        const char *v = argv[++i];
        if (!strcmp(a, "--header")) { if (nheaders == MAXN) die("too many --header"); headers[nheaders++] = v; }
        else if (!strcmp(a, "--root")) { if (nroots == MAXN) die("too many --root"); roots[nroots++] = v; }
        else if (!strcmp(a, "--const")) { if (nprefixes == MAXN) die("too many --const"); prefixes[nprefixes++] = v; }
        else if (!strcmp(a, "--snapshot")) { if (nsnaps == MAXN) die("too many --snapshot"); snaps[nsnaps++] = v; }
        else if (!strcmp(a, "--count")) {
            if (ncounts == MAXN) die("too many --count");
            Count *c = &counts[ncounts++];
            char *dot, *eq = NULL;
            c->type = xstrdup(v);
            if (!(dot = strchr(c->type, '.')) || !(eq = strchr(dot, '=')) || dot == c->type || eq == dot + 1 || !eq[1])
                die("--count must be TYPE.field=count_field");
            *dot = 0; *eq = 0;
            c->field = dot + 1; c->count = eq + 1;
        }
        else if (!strcmp(a, "--build")) { if (build) die("one --build per run (run once per build)"); build = v; }
        else if (!strcmp(a, "--ts")) out_ts = v;
        else if (!strcmp(a, "--hash-ts")) out_hash_ts = v;
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
    if (!out_ts && !out_hash_ts && !print_hash) die("nothing to do: give --ts, --hash-ts and/or --print-hash");
    const char *eq = strchr(build, '=');
    if (!eq || eq == build) die("--build must be NAME=FLAGS");
    build_name_end = eq;
    out_ts = absolute(out_ts);   // output paths are relative to where we were started, not to --cwd
    out_hash_ts = absolute(out_hash_ts);
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

    if (snapshot_only && !nsnaps) die("--snapshot-only without a --snapshot");
    for (int i = 0; i < ncounts; i++) {
        Count *c = &counts[i];
        Rec *r = NULL;
        for (int j = 0; j < nrecs; j++) if (recs[j].record && !strcmp(recs[j].name, c->type)) r = &recs[j];
        if (!r) die("--count %s.%s: no such record reached from the roots", c->type, c->field);
        Field *f = field_named(r, c->field), *cf = field_named(r, c->count);
        if (!f || f->width || f->nd != 1) die("--count %s.%s: not a one-dimensional array field", c->type, c->field);
        if (!cf || cf->width || cf->nd || recs[cf->type].record || recs[cf->type].kind == 'f' || recs[cf->type].size > 4)
            die("--count %s.%s=%s: the count is not an integer field of %s", c->type, c->field, c->count, c->type);
    }
    for (int i = 0; i < nsnaps; i++) {
        int found = -1;
        for (int j = 0; j < nrecs; j++) if (recs[j].record && !strcmp(recs[j].name, snaps[i])) found = j;
        if (found < 0) die("--snapshot %s: no such record reached from the roots", snaps[i]);
        snap_mark(found);
    }
    for (int i = 0; i < ncounts; i++) {
        int used = 0;
        for (int j = 0; j < nrecs; j++) used |= recs[j].snap && !strcmp(recs[j].name, counts[i].type);
        if (!used) die("--count %s.%s: %s is not in any snapshot", counts[i].type, counts[i].field, counts[i].type);
    }

    // ---- the module body, and the layout hash beside it -----------------------
    Buf ts = {0};
    int strings = 0;
    if (nconsts) bprintf(&ts, "// constants\n");
    for (int p = 0; p < nprefixes; p++)
        for (int m = 1; m >= 0; m--)
            for (int j = 0; j < nconsts; j++)
                if (consts[j].prefix == p && consts[j].macro == m) {
                    bprintf(&ts, "export const %s = %lld;\n", consts[j].name, consts[j].value);
                    hput("const %s %lld", consts[j].name, consts[j].value);
                }
    if (!snapshot_only) for (int i = 0; i < nrecs; i++) if (recs[i].record) emit_record(&ts, &recs[i], &strings);
    for (int i = 0; i < nrecs; i++) if (recs[i].snap) emit_snapshot(&ts, &recs[i], &strings);
    check_unique_exports(&ts);
    for (int i = 0; i < nroots; i++) hash_record(root_rec[i], roots[i]);
    unsigned hash = layout_hash;

    if (out_ts) {
        FILE *fp = fopen(out_ts, "w");
        if (!fp) die("cannot write %s", out_ts);
        emit_banner(fp);
        fputs("export interface Mem { u8: Uint8Array; i8: Int8Array; dv: DataView }\n"
              "export const memOf = (b: ArrayBuffer): Mem => ({ u8: new Uint8Array(b), i8: new Int8Array(b), dv: new DataView(b) });\n", fp);
        if (strings & STR_ACCESSORS)   // char[N] as UTF-8 + NUL; ASCII stays off TextEncoder/TextDecoder
            fputs("const utf8Enc = /* @__PURE__ */ new TextEncoder(), utf8Dec = /* @__PURE__ */ new TextDecoder();\n", fp);
        else if (strings) fputs("const utf8Dec = /* @__PURE__ */ new TextDecoder();\n", fp);
        if (strings & (STR_ACCESSORS | STR_CSTR_GET)) fputs(
            "const cstrGet = (m: Mem, a: number, n: number) => {\n"
            "    let s = '', e = a;\n"
            "    for (const end = a + n; e < end; e++) { const c = m.u8[e]; if (c === 0) return s; if (c > 127) break; s += String.fromCharCode(c); }\n"
            "    while (e < a + n && m.u8[e] !== 0) e++;\n"
            "    return e === a + n && s.length === n ? s : utf8Dec.decode(m.u8.subarray(a, e));\n"
            "};\n", fp);
        if (strings & STR_ACCESSORS) fputs(
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
        if (strings & STR_UTF8_GET) fputs(   // exactly n bytes of UTF-8 (a counted char array)
            "const utf8Get = (m: Mem, a: number, n: number) => {\n"
            "    if (n > 16) return utf8Dec.decode(m.u8.subarray(a, a + n));   // past a few bytes the decoder wins\n"
            "    let s = '';\n"
            "    for (let i = 0; i < n; i++) { const c = m.u8[a + i]; if (c > 127) return utf8Dec.decode(m.u8.subarray(a, a + n)); s += String.fromCharCode(c); }\n"
            "    return s;\n"
            "};\n", fp);
        fwrite(ts.s, 1, ts.n, fp);
        if (fclose(fp)) die("cannot write %s", out_ts);
    }
    if (out_hash_ts) {
        FILE *fp = fopen(out_hash_ts, "w");
        if (!fp) die("cannot write %s", out_hash_ts);
        emit_banner(fp);
        fprintf(fp, "export const LAYOUT_HASH = 0x%08x;\n", hash);
        if (fclose(fp)) die("cannot write %s", out_hash_ts);
    }
    if (print_hash) printf("0x%08x\n", hash);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
    return 0;
}
