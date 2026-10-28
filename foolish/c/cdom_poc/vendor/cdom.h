/* cdom: C owns the tree, state and diffing; a fixed JS shim applies a
 * batched binary op stream to the real DOM and feeds events back. */
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

enum { CD_EV_CLICK, CD_EV_DBLCLICK, CD_EV_INPUT, CD_EV_CHANGE, CD_EV_KEYDOWN, CD_EV_KEYUP,
       CD_EV_FOCUSIN, CD_EV_FOCUSOUT, CD_EV_SUBMIT, CD_EV_POINTERDOWN, CD_EV_POINTERUP,
       CD_EV_POINTERMOVE };

/* memory */
void *cd_alloc(uint32_t n);
void *cd_realloc(void *p, uint32_t n);
void cd_free(void *p);

/* strings */
cd_str cd_intern(const char *s, uint32_t len);
cd_str cd_internz(const char *s);
const char *cd_bytes(cd_str s, uint32_t *len);

/* node ids */
cd_id cd_id_new(void);
void cd_id_free(cd_id id);           /* JS drops its reference; no DOM change */
void cd_tag_set(cd_id id, uint32_t tag); /* app data per node, for events */
uint32_t cd_tag(cd_id id);

/* building: nodes go to (parent, before) when their root closes */
void cd_at(cd_id parent, cd_id before); /* before 0 = append; parent CD_DETACHED = keep off-DOM */
void cd_open(cd_str tag);
void cd_close(void);
void cd_text(cd_str s);
void cd_text_num(uint32_t n);
void cd_attr(cd_str name, cd_str value);
void cd_class(cd_str value);
void cd_name(cd_id id);              /* remember the last node as id */
void cd_listen(cd_id id, uint32_t ev); /* name the last node and deliver ev for it */
void cd_value(cd_str s);             /* last.nodeValue */
void cd_value_num(uint32_t n);
void cd_tpl_save(uint32_t t);        /* the last closed root becomes template t */
void cd_clone(uint32_t t);           /* open a deep clone of template t */
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

/* updating */
void cd_set_text(cd_id id, cd_str s); /* id is a text node */
void cd_set_text_num(cd_id id, uint32_t n);
void cd_set_attr(cd_id id, cd_str name, cd_str value);
void cd_set_class(cd_id id, cd_str value);
void cd_remove(cd_id id);            /* detach and free the id */
void cd_move(cd_id id, cd_id before);
void cd_clear(cd_id id);             /* drop all children; caller frees their ids */
void cd_detach(cd_id id);            /* take a node out of the document, remembering its place */
void cd_attach(cd_id id);            /* put it back where it was */
void cd_flush(void);

/* events: the app implements this; the shim fills the record */
/* id is the nearest listening ancestor-or-self of the target. path holds the
 * element-child indices from that node down toward the target, one byte per
 * level (byte 0 = which child of the listener), for depth levels (max 4). */
typedef struct { int32_t type, id, x, y, button, key, mods, path, depth; } cd_event_rec;
void cd_on_event(const cd_event_rec *e);
void cd_main(void);                  /* the app's entry, called once at start */
uint32_t cd_input_value(cd_id id, char *dst, uint32_t cap); /* copy an input's value on demand */

/* keyed list */
typedef struct { uint32_t key; cd_id node; uint32_t data; } cd_item;
/* owns_parent: the list is the parent's only content, so a full clear is one op
 * and a large first fill builds off-document. */
typedef struct { cd_item *items; uint32_t n, cap; cd_id parent; int owns_parent; } cd_list;
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

#endif
