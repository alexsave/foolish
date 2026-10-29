/* cdom: C owns the tree, state and diffing; a fixed JS shim makes one DOM call per
 * operation (each its own wasm import) and feeds events back. */
#ifndef CDOM_H
#define CDOM_H
#include <stdint.h>

/* Content strings (text, class values) cross once and are cached in JS by id
 * when 1; when 0 every use re-sends the bytes. Tag and attribute names are
 * always cached: they are the enum. */
#ifndef CDOM_INTERN_TEXT
#define CDOM_INTERN_TEXT 1
#endif

typedef uint32_t cd_id;  /* node id; 0 is the mount element */
typedef uint32_t cd_str; /* interned string id */

#define CD_MOUNT 0u
#define CD_DETACHED 0xffffffffu

/* CD_EV_RESIZE and CD_EV_VISIBILITY are window events (cd_listen_window); every other type can be
 * listened for on a node (cd_listen) or on the whole document (cd_listen_window). The mouse and
 * touch types are for a drag as the browser has always delivered it: the press on the node, the
 * moves and the release on the document while it lasts. */
enum { CD_EV_CLICK, CD_EV_DBLCLICK, CD_EV_INPUT, CD_EV_CHANGE, CD_EV_KEYDOWN, CD_EV_KEYUP,
       CD_EV_FOCUSIN, CD_EV_FOCUSOUT, CD_EV_SUBMIT, CD_EV_POINTERDOWN, CD_EV_POINTERUP,
       CD_EV_POINTERMOVE, CD_EV_RESIZE, CD_EV_VISIBILITY, CD_EV_MOUSEDOWN, CD_EV_MOUSEMOVE,
       CD_EV_MOUSEUP, CD_EV_TOUCHSTART, CD_EV_TOUCHMOVE, CD_EV_TOUCHEND, CD_EV_TOUCHCANCEL };

/* The event record's mods bits. CD_MOD_EDITING: the event's target is an input, a textarea, a
 * select or contenteditable (a key typed into a field, which an app's shortcuts leave alone). */
enum { CD_MOD_SHIFT = 1, CD_MOD_CTRL = 2, CD_MOD_ALT = 4, CD_MOD_META = 8, CD_MOD_EDITING = 16 };

/* ---- memory ----
 * cd_alloc and friends go through malloc, realloc and free. cdom defines those (and calloc)
 * as weak symbols over its own size-class heap from __heap_base, so an app with no allocator
 * gets cdom's, and the app's malloc is the same heap. An app that brings an allocator defines
 * all four, strong, and cdom then allocates from it: one heap either way, never two
 * allocators that both start at __heap_base. cdom's free and realloc trap on a pointer their
 * heap did not hand out, so an app that replaces malloc alone fails loudly, not silently. */
void *cd_alloc(uint32_t n);
void *cd_realloc(void *p, uint32_t n);
void cd_free(void *p);

/* ---- strings ---- */
cd_str cd_intern(const char *s, uint32_t len);
cd_str cd_internz(const char *s);
const char *cd_bytes(cd_str s, uint32_t *len);
/* A one-off string: its bytes cross with the next op and are never interned or cached, so a
 * value that changes (a position, a count in a label) does not grow the string table. Use it
 * in the very next op that takes a string; using it after another op has crossed traps. */
cd_str cd_tmp(const char *s, uint32_t len);
cd_str cd_tmpz(const char *s);
/* A string literal interned once per call site: the hash lookup is paid the first time only.
 * ONLY A LITERAL: the cache is per call site, so a variable would answer its first value
 * forever. `"" lit` makes anything but a string literal a compile error. The check is in a
 * helper, not inline: inlined at every site it cost an app 5.5 KB. */
cd_str cd_lit_(cd_str *slot, const char *lit, uint32_t len);
#define CD_LIT(lit) ({ static cd_str cd_lit_slot_; cd_lit_(&cd_lit_slot_, "" lit, sizeof(lit) - 1); })
/* The string table's size: C's pool and the shim's array hold the same strings. */
uint32_t cd_str_count(void);
uint32_t cd_str_bytes(void);

/* Numbers as JS and CSS write them, with no printf. cd_fmt writes v / 10^decimals (decimals <= 9)
 * as JS's String(v / 10 ** decimals) does (trailing zeros dropped, no "-0"), then the unit, and
 * returns the bytes written (at most 24). cd_fmt_len rounds a double to 4 decimals first. */
enum { CD_UNIT_NONE, CD_UNIT_PX, CD_UNIT_PCT, CD_UNIT_DEG, CD_UNIT_EM, CD_UNIT_REM, CD_UNIT_VW,
       CD_UNIT_VH, CD_UNIT_MS, CD_UNIT_S, CD_UNIT_RAD, CD_UNITS };
uint32_t cd_fmt(char *dst, int32_t v, uint32_t decimals, uint32_t unit);
uint32_t cd_fmt_len(char *dst, double v, uint32_t unit);

/* ---- node ids ---- */
cd_id cd_id_new(void);
void cd_id_free(cd_id id);           /* JS drops its reference; no DOM change. Freeing a free id traps. */
void cd_tag_set(cd_id id, uint32_t tag); /* app data per node, for events */
uint32_t cd_tag(cd_id id);
uint32_t cd_ids_live(void);          /* ids handed out and not freed */
uint32_t cd_ids_high(void);          /* the highest id ever handed out: the size of the shim's node table */

/* ---- building: nodes go to (parent, before) when their root closes ---- */
void cd_at(cd_id parent, cd_id before); /* before 0 = append; parent CD_DETACHED = keep off-DOM */
/* An <svg>, and everything opened inside it, is made in the SVG namespace (C keeps the depth). */
void cd_open(cd_str tag);
void cd_close(void);
void cd_text(cd_str s);
void cd_text_num(uint32_t n);
void cd_attr(cd_str name, cd_str value);
void cd_class(cd_str value);
/* cd_name names the LAST NODE MADE: after cd_open that is the element, after cd_text the text
 * node, but after a cd_close it is the deepest node built inside, not the element closed.
 * cd_name_open names the innermost open element, whatever has been built inside it so far. */
void cd_name(cd_id id);
void cd_name_open(cd_id id);
void cd_listen(cd_id id, uint32_t ev); /* name the last node and deliver ev for it */
void cd_value(cd_str s);             /* last.nodeValue */
void cd_value_num(uint32_t n);
void cd_tpl_save(uint32_t t);        /* the last closed root becomes template t */
void cd_clone(uint32_t t);           /* open a deep clone of template t (a child of the open element, if any) */
/* Template slots: while a template is being built, cd_slot(k) marks the last node (a text node)
 * as slot k (k < CD_SLOTS). cd_tpl_text sets that text in template t itself, so the following
 * cd_clone(t) copies it in and the clone needs no walk. A slot keeps its last value: set every
 * slot before each clone. t < CD_TPLS. */
#define CD_TPLS 64
#define CD_SLOTS 16
void cd_slot(uint32_t k);
void cd_tpl_text(uint32_t t, uint32_t k, cd_str s);
void cd_tpl_text_num(uint32_t t, uint32_t k, uint32_t n);
void cd_down(void);                  /* last = last.firstChild */
void cd_next(void);                  /* last = last.nextSibling */
void cd_up(void);                    /* last = last.parentNode */
void cd_sel(cd_id id);               /* last = the node named id; then walk with down/next/up */

/* ---- updating ---- */
void cd_set_text(cd_id id, cd_str s); /* id is a text node */
void cd_set_text_num(cd_id id, uint32_t n);
void cd_set_attr(cd_id id, cd_str name, cd_str value);
void cd_set_class(cd_id id, cd_str value);
/* One style property: element.style.setProperty(prop, value). value may be a cd_tmp; "" removes it. */
void cd_set_style(cd_id id, cd_str prop, cd_str value);
void cd_set_style_num(cd_id id, cd_str prop, double v, uint32_t unit);
/* Pixels to an image file: px is w * h * n bytes in C memory, n = 4 (RGBA) or 3 (RGB, opaque: a
 * third less room for a picture with no alpha), read during the call (reuse it after). The shim
 * encodes them as mime ("image/png", or "image/jpeg" at quality 0.92) off the
 * call; the file's bytes arrive in cd_on_encoded(token, p, len), valid until it returns. */
void cd_encode_image(const uint8_t *px, uint32_t w, uint32_t h, uint32_t n, cd_str mime, uint32_t token);
/* The same, the pixels placed at (x, y) in an image of W x H whose rest is transparent (black in a
 * JPEG): for a picture of which only a part is ever shown, but whose size the page lays out by. */
void cd_encode_image_in(const uint8_t *px, uint32_t w, uint32_t h, uint32_t n, uint32_t W, uint32_t H, uint32_t x,
                        uint32_t y, cd_str mime, uint32_t token);
void cd_on_encoded(uint32_t token, const uint8_t *p, uint32_t len); /* the app's hook (empty weak default) */
/* An image file's bytes (len of them at p, read during the call) as a CSS image: style property
 * prop of node id becomes url(blob:...) once the image has decoded, so no frame paints the
 * property before its picture (a custom property such as "--tex" works), and then
 * cd_on_image(token, 1) is called, in the same task: what goes with the picture (a colour under
 * it) is set there and paints with it. Bytes that do not decode set nothing: cd_on_image(token, 0). */
void cd_set_image_bytes(cd_id id, cd_str prop, const uint8_t *p, uint32_t len, cd_str mime, uint32_t token);
void cd_on_image(uint32_t token, uint32_t ok); /* the app's hook (empty weak default) */
void cd_remove(cd_id id);            /* detach and free the id */
void cd_move(cd_id id, cd_id before);
void cd_clear(cd_id id);             /* drop all children; caller frees their ids (a cd_region does it for you) */
void cd_detach(cd_id id);            /* take a node out of the document, remembering its place */
void cd_attach(cd_id id);            /* put it back where it was */
/* Sends pending strings and any held walk. Every entry from JS (start, events, timers, frames,
 * the network) flushes on its way out, so an app calls this only from an export of its own. */
void cd_flush(void);
uint32_t cd_pending(void);           /* nonzero while strings or a walk wait to cross (for tests) */

/* ---- events: the app implements cd_on_event; the shim fills the record ----
 * id is the nearest listening ancestor-or-self of the target. path holds the
 * element-child indices from that node down toward the target, one byte per
 * level (byte 0 = which child of the listener), for depth levels (max 4).
 * x, y: clientX and clientY (a touch event's: its first changed touch, the finger that pressed,
 * moved or lifted). key: keyCode. ch: the key's character when event.key is one UTF-16 unit
 * ("a", "A", "3", "-", " "), else 0 ("ArrowUp", "Shift"). mods: CD_MOD_* bits. ms: when the
 * event happened (event.timeStamp, cd_now()'s clock, whole ms): a press and its release are timed
 * by their own stamps, not by when their handlers got to run. prevent is cdom's (cd_prevent_default).
 * A window event (CD_EV_RESIZE, CD_EV_VISIBILITY) has id CD_MOUNT, x and y the viewport's
 * size, and key 1 when the document is hidden; any other type listened for with cd_listen_window
 * is the document's event as it is, with id CD_MOUNT (cd_hit names the node under a point). */
typedef struct { int32_t type, id, x, y, button, key, mods, path, depth, ch, prevent, ms; } cd_event_rec;
void cd_on_event(const cd_event_rec *e);
void cd_main(void);                  /* the app's entry, called once at start */
uint32_t cd_input_value(cd_id id, char *dst, uint32_t cap); /* copy an input's value on demand */
/* Deliver ev for the whole page: CD_EV_RESIZE on the window, every other type on the document
 * (keys, and the moves and release of a drag). Listening twice is listening once. The listener
 * is not passive, so cd_prevent_default works on a touchmove (it stops the page scrolling). */
void cd_listen_window(uint32_t ev);
void cd_unlisten_window(uint32_t ev); /* stop delivering it (a drag that ended) */
/* During cd_on_event: the browser's default action for this event is not taken (event.preventDefault
 * once the hook returns): no native drag after a mousedown, no scroll after a touchmove, no mouse
 * events and click after a touchend, no character typed after a keydown. */
void cd_prevent_default(void);
/* The node an event at (x, y) in the viewport would be delivered to: the nearest listening
 * ancestor-or-self of the topmost element there that takes pointer events (elementFromPoint, so
 * a pointer-events: none overlay is seen through), whatever type it listens for; 0 for none. The
 * browser lays the page out first if anything changed. */
cd_id cd_hit(int32_t x, int32_t y);
typedef struct { int32_t w, h, hidden, dpr100; } cd_viewport_rec;
/* innerWidth, innerHeight, document.hidden and devicePixelRatio (times 100, rounded), read now */
void cd_viewport(cd_viewport_rec *v);
/* A node's box on screen now (getBoundingClientRect: with its transforms, in CSS px from the
 * viewport's top left): out[0] left, out[1] top, out[2] width, out[3] height. The browser lays the
 * page out first if anything changed, so measure once per change, never every frame. */
void cd_rect(cd_id id, double out[4]);

/* ---- time: answered through the app's hooks (cdom has empty weak defaults) ---- */
void cd_timer_start(uint32_t ms, uint32_t token); /* cd_on_timer(token) after ms */
void cd_raf_request(uint32_t token);              /* cd_on_raf(token, t) before the next frame */
void cd_on_timer(uint32_t token);
void cd_on_raf(uint32_t token, double t);
double cd_now(void);                              /* performance.now(), for work cut into slices */

/* ---- regions: children that come and go ----
 *   if (cd_region_show(&r, want)) { cd_at(...); ...build...; cd_region_end(&r); }
 * cd_region_show returns 1 when the region must be built now (it is hidden and want is set);
 * with want 0 and the region shown, it takes the region away. While a region builds, every id
 * cd_id_new makes is recorded in it, and taking the region away frees them all.
 * Two kinds, by r->parent, set once before first use:
 * - r->parent != 0: the region is ALL the children of that (named, non-mount) node;
 *   taking it away is one clear.
 * - r->parent == 0: the region is one element, which the builder names cd_name_open(r->root)
 *   (cd_region_show makes r->root); taking it away is one remove.
 * Lists updated while a region builds keep their own ids (cd_list_update does not record);
 * drop a list that lived inside a region with cd_list_forget. Regions do not nest (traps). */
typedef struct cd_region { cd_id parent, root; cd_id *ids; uint32_t n, cap; int shown; } cd_region;
int cd_region_show(cd_region *r, int want);
void cd_region_end(cd_region *r);
void cd_region_hide(cd_region *r);   /* = cd_region_show(r, 0) */

/* ---- keyed list ---- */
typedef struct { uint32_t key; cd_id node; uint32_t data; } cd_item;
/* owns_parent: the list is the parent's only content, so a full clear is one op
 * and a large first fill builds off-document.
 * end: when fixed siblings follow the list in its parent, the first of them; items at the
 * list's tail go before it (0: the list runs to the parent's end). Only with owns_parent 0. */
typedef struct { cd_item *items; uint32_t n, cap; cd_id parent; int owns_parent; cd_id end; } cd_list;
typedef struct {
  void *ctx;
  /* emit the subtree for new item i (cd_at is already set); return its root id */
  cd_id (*create)(void *ctx, uint32_t i, uint32_t *data);
  /* item i survived: emit only what changed */
  void (*patch)(void *ctx, uint32_t i, cd_item *it);
  /* item is gone (its root id is already freed): free the sub ids it named */
  void (*destroy)(void *ctx, cd_item *it);
} cd_list_fns;
void cd_list_update(cd_list *L, const uint32_t *keys, uint32_t n, const cd_list_fns *f);
/* The list's DOM already went (an ancestor was cleared or removed): free every item's ids,
 * emit nothing for their nodes, and leave the list empty. */
void cd_list_forget(cd_list *L, const cd_list_fns *f);

#endif
