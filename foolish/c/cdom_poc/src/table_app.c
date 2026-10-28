/* table_app.c - foolish's game table, drawn from C through cdom.
 *
 * The board is the kernel's: a Game, read for the viewer through the client
 * slot (client_table.h client_adopt_board) into a TableView and its ViewRules,
 * played on with client_play (what a selection means), client_move_wire and
 * awire_apply, and answered by the handwritten bot through bot_drive. This
 * file is only the screen: it draws the TableView with the look of the React
 * table it stands in for (src/components/GameBoard.tsx and the GameDisplay components), as
 * inline styles copied from those components over the app's own CSS, and it
 * turns clicks into moves.
 *
 * Every region keeps what it last drew and emits only the difference: the
 * hand, the battles, the pills, the stock and discard layers and each seat's
 * mini hand are cdom keyed lists, and the counts and role marks are patched in
 * place. Nothing is rebuilt on a move. */
#include "ui.h"
#include "game.h"
#include "legal.h"
#include "awire.h"
#include "bot_drive.h"
#include "client_table.h"
#include "hand_layout.h"
#include "anim_plan.h"
#include "strategy.h"
#include "../cardtext/card_text.h"
#include "textures.h"
#include "cdom_store.h"
#include "game_keep.h"
#include <math.h>


#define EXPORT(n) __attribute__((export_name(n)))
/* The page's one setting: 1 when the URL asks for no textures (the golden comparisons). */
__attribute__((import_module("env"), import_name("notex"))) uint32_t page_notex(void);

extern uint32_t cd_seed;

/* ---- the game ------------------------------------------------------------ */

#define ME 0

static Game G;
static ClientSlot SLOT;
static ClientTable CT;
static ViewRules R;
static LegalMoves MOVES;
static unsigned char MENU[64 * 1024];
static const TableView *V;

static const char *const SEAT_NAME[] = {"Me", "Anna"};
static const char *const SEAT_ID[] = {"u-me-0000", "u-anna-0001"};

/* The selection: card ids (card.h card_to_id) in the order they were picked. */
static uint8_t sel[MAX_MOVE_CARDS];
static int nsel;
static int bot_pending;   /* the bot may act: its timer is running */
static uint32_t pressed, prev_rearm; /* ActionButtons.tsx pressedActions, and the pills it re-arms on (render_hand) */
static int last_reject;   /* the engine's reason for the last refused move, for the test */
static uint32_t rng;

static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int parse(const char *s, Card *out, Card *covers, int cap) {
  int n = 0;
  while (s[n]) n++;
  return card_list_parse(s, n, out, covers, cap);
}

/* The board e2e/ui_dom_snapshots.test.ts twoSeatBout() composes for the golden
 * bout_2p_attacker.html: the same fields its fixture builder writes. */
static void deal_fixture(void) {
  __builtin_memset(&G, 0, sizeof G);
  game_reset_to_lobby(&G, 0);
  Card c[64], cov[64];
  G.status = GAME_STATUS_PLAYING;
  G.num_players = 2;
  parse("Kc", c, 0, 1);
  G.power_suit = c[0].suit;
  G.has_flipped = true;
  G.flipped = c[0];
  G.first_attacker = 0;
  G.defender = 1;
  int n = parse("7s 8s 9s Ts Js Qs Ks As 7c 8c 9c Tc Jc Qc Ac 7d 8d 9d Td Jd", c, 0, MAX_DECK);
  G.deck_count = (int16_t)n;
  for (int i = 0; i < n; i++) G.deck[i] = c[i];
  n = parse("6h/8h 6d", c, cov, MAX_BATTLES);
  G.num_battles = (int8_t)n;
  for (int i = 0; i < n; i++) G.table_battles[i] = (Battle){c[i], cov[i]};
  const char *hands[] = {"6s 7h Qd Ad", "9h Th Jh 6c"};
  for (int s = 0; s < 2; s++) {
    Player *p = &G.players[s];
    n = parse(hands[s], c, 0, MAX_HAND_SIZE);
    p->status = PLAYER_STATUS_IN;
    p->hand_count = (int8_t)n;
    for (int i = 0; i < n; i++) p->hand[i] = c[i];
  }
  G.players[0].strategy_key = STRATEGY_KEY_HUMAN;
  G.players[1].strategy_key = STRAT_HANDWRITTEN_PROD;
}

static void deal_new(void) {
  __builtin_memset(&G, 0, sizeof G);
  game_reset_to_lobby(&G, 0);
  game_set_seed(rnd() | 1);
  const int8_t keys[2] = {STRATEGY_KEY_HUMAN, STRAT_HANDWRITTEN_PROD};
  game_seat_and_deal(&G, keys, 2);
}

/* The textures (TEX_ORDER: the order they are made). The wood is two pictures of the same tile:
 * the surfaces on the table (the back button, the pills), and those of the end screen, which is
 * made last and shown much later; each is worked out only where its surfaces are. */
enum { TEX_FERN, TEX_WOOD, TEX_WOOL, TEX_WOOD_END, TEX_DONE, NTEX = TEX_DONE };

static int my_turn_owed(void) { return bot_drive_eligible_mask(&G, game_human_mask(&G)) == 0; }

/* ---- strings ------------------------------------------------------------- */

static cd_str S_STYLE, S_RANK[ACE_VALUE + 1], S_SUIT[NUM_SUITS];

static int card_red(Card c) { return CARD_SUIT_FACE[c.suit].red; }

/* ---- card faces (CardFace.tsx) -------------------------------------------
 * A face is a template: the corner indices and the centre glyph are slots set
 * in the template, then one clone. "10" has its own template because its rank
 * is drawn tighter; a card squeezed under 40px wide has the thin layout. */
enum { T_FACE = 1, T_FACE10, T_THIN };

#define CORNER "width: 12px; position: absolute; font-size: 20px; display: flex; flex-direction: column; " \
  "align-items: center; justify-content: center; text-align: center; user-select: none; -webkit-user-select: none; "

static void build_face_template(uint32_t t, int ten) {
  cd_at(CD_DETACHED, 0);
  OPEN("div");
  for (int corner = 0; corner < 2; corner++) {
    OPEN("div");
    if (corner) STYLE(CORNER "bottom: 4px; right: 4px; transform: rotate(180deg);");
    else STYLE(CORNER "left: 4px; top: 4px;");
    OPEN("div");
    if (ten) STYLE("letter-spacing: -0.08em; margin-left: 3px;");
    TEXT(" "); cd_slot((uint32_t)corner * 2);
    CLOSE();
    OPEN("div"); OPEN("span"); STYLE("font-size: 14px;"); TEXT(" "); cd_slot((uint32_t)corner * 2 + 1); CLOSE(); CLOSE();
    CLOSE();
  }
  OPEN("div");
  STYLE("position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); pointer-events: none; "
        "user-select: none; -webkit-user-select: none; text-align: center; line-height: 1;");
  OPEN("div"); OPEN("span"); STYLE("font-size: 32px;"); TEXT(" "); cd_slot(4); CLOSE(); CLOSE();
  CLOSE();
  CLOSE();
  cd_tpl_save(t);
}

static void build_thin_template(void) {
  cd_at(CD_DETACHED, 0);
  OPEN("div");
  OPEN("div"); STYLE("font-size: 20px;"); TEXT(" "); cd_slot(0); CLOSE();
  OPEN("div"); OPEN("span"); STYLE("font-size: 20px;"); TEXT(" "); cd_slot(1); CLOSE(); CLOSE();
  CLOSE();
  cd_tpl_save(T_THIN);
}

/* Opens a face of c (the caller adds its style, built after this, and closes it: a style is a
 * one-off string that must cross with the very next op). */
static void face_open(Card c, int thin) {
  cd_str r = S_RANK[c.value], s = S_SUIT[c.suit];
  if (thin) {
    cd_tpl_text(T_THIN, 0, r);
    cd_tpl_text(T_THIN, 1, s);
    cd_clone(T_THIN);
  } else {
    uint32_t t = c.value == 9 ? T_FACE10 : T_FACE;
    cd_tpl_text(t, 0, r); cd_tpl_text(t, 1, s);
    cd_tpl_text(t, 2, r); cd_tpl_text(t, 3, s);
    cd_tpl_text(t, 4, s);
    cd_clone(t);
  }
}

enum { FACE_STILL, FACE_HAND, FACE_ATTACK, FACE_DEFENSE };
#define COVER_TILT "0.19634954084936207rad" /* TableBattles.tsx COVER_ROTATION_RAD, pi / 16 */

static cd_str face_style(Card c, int kind, const HandRect *slot, double bottom, int selected, int tilted) {
  static SB s;
  sb_reset(&s);
  sb_s(&s, "background-color: var(--color-card-face); ");
  if (kind == FACE_HAND) { sb_px(&s, "width: ", slot->w); sb_px(&s, "height: ", slot->h); }
  else sb_s(&s, "width: 50px; height: 70px; ");
  sb_s(&s, "border-radius: 5px; border: 2px solid ");
  sb_s(&s, selected ? "red; " : "black; ");
  sb_s(&s, kind == FACE_HAND ? "cursor: move; " : kind == FACE_ATTACK ? "cursor: pointer; " : "cursor: default; ");
  sb_s(&s, "user-select: none; -webkit-user-select: none; ");
  sb_s(&s, kind == FACE_HAND || kind == FACE_ATTACK ? "pointer-events: auto; " : "pointer-events: none; ");
  sb_s(&s, kind == FACE_HAND ? "transition: all 0.1s ease, visibility 0s; "
         : kind == FACE_ATTACK ? "transition: transform 0ms cubic-bezier(0.25, 0.46, 0.45, 0.94) 0ms, box-shadow 0.2s ease-in-out; "
         : "transition: transform 0.2s ease-in-out, opacity 0.2s ease-in-out, box-shadow 0.2s ease-in-out; ");
  sb_s(&s, kind == FACE_STILL ? "position: relative; " : "position: absolute; ");
  sb_s(&s, "font-family: Georgia, serif; font-weight: bold; color: ");
  sb_s(&s, card_red(c) ? "rgb(220, 38, 38); " : "rgb(0, 0, 0); ");
  sb_s(&s, "display: flex; flex-direction: column; font-size: 20px; line-height: 15px; ");
  if (kind == FACE_HAND) {
    sb_px(&s, "left: ", slot->x);
    sb_px(&s, "bottom: ", bottom);
    sb_s(&s, "box-sizing: border-box; z-index: 1000; justify-content: center; align-items: center; opacity: 1;");
  } else if (kind == FACE_ATTACK) {
    sb_s(&s, tilted ? "bottom: 5px; left: 50%; transform: translateX(-50%) rotate(-" COVER_TILT "); transform-origin: center bottom; z-index: 1;"
                    : "bottom: 5px; left: 50%; transform: translateX(-50%); transform-origin: center bottom; z-index: 2;");
  } else if (kind == FACE_DEFENSE) {
    sb_s(&s, "bottom: 5px; left: 50%; transform: translateX(-50%) rotate(" COVER_TILT "); transform-origin: center bottom; z-index: 2;");
  }
  return sb_tmp(&s);
}

static void data_card(Card c) {
  char b[8] = {(char)('0' + c.suit), '-'};
  uint32_t n = 2 + cd_fmt(b + 2, c.value, 0, CD_UNIT_NONE);
  cd_attr(ZS("data-card"), cd_tmp(b, n));
}

/* ---- card backs (CardBack.tsx, the plain red the golden shows) ----------- */

static cd_str back_style(uint32_t i, double rot, int stacked) {
  static SB s;
  sb_reset(&s);
  sb_s(&s, "position: absolute; ");
  sb_px(&s, "top: ", stacked ? -2.0 * i : 0);
  sb_px(&s, "left: ", stacked ? -1.0 * i : 0);
  sb_s(&s, "width: 50px; height: 70px; background-color: var(--tex-fern-bg, rgb(180, 14, 9)); "
           "border: 1px solid rgb(139, 0, 0); border-radius: 5px; ");
  sb_s(&s, "z-index: "); sb_i(&s, i + 2);
  sb_s(&s, "; transform: rotate(90deg) rotate("); sb_num(&s, rot, CD_UNIT_NONE);
  sb_s(&s, "deg); transform-origin: center center; background-image: var(--tex-fern); background-size: 100% 100%; "
           "background-repeat: no-repeat; overflow: hidden;");
  return sb_tmp(&s);
}

/* CardBack.tsx getRandomRotation(42, i): the discard pile's tilt per layer. */
static double discard_tilt(uint32_t i) {
  double x = sin(42.0 + i * 1000.0) * 10000.0;
  return (x - __builtin_floor(x)) * 40.0 - 20.0;
}

static cd_id layer_create(void *ctx, uint32_t i, uint32_t *data) {
  int stacked = ctx != 0;
  cd_id id = cd_id_new();
  OPEN("div");
  cd_name_open(id);
  cd_attr(S_STYLE, back_style(i, stacked ? 0 : discard_tilt(i), stacked));
  CLOSE();
  *data = 0;
  return id;
}
static void layer_patch(void *ctx, uint32_t i, cd_item *it) { (void)ctx; (void)i; (void)it; }
static void layer_destroy(void *ctx, cd_item *it) { (void)ctx; (void)it; }
static const cd_list_fns DECK_LAYERS = {(void *)1, layer_create, layer_patch, layer_destroy};
static const cd_list_fns DISCARD_LAYERS = {0, layer_create, layer_patch, layer_destroy};

static uint32_t keys[128];
static void layers_update(cd_list *L, uint32_t n, const cd_list_fns *f) {
  for (uint32_t i = 0; i < n; i++) keys[i] = i + 1;
  cd_list_update(L, keys, n, f);
}

#define BADGE "position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); margin: 0px; " \
  "color: var(--color-text-primary); font-size: 16px; font-weight: bold; text-shadow: 1px 1px 2px rgba(0,0,0,0.8); " \
  "pointer-events: none; z-index: 1003;"

/* ---- the stock and the trump (DeckAndFlipped.tsx) ------------------------ */

static cd_id deck_badge_txt, deck_layers_box;
static cd_region deck_area; /* all of the deck's column: rebuilt when what it holds changes */
static cd_list deck_layers;
static int32_t deck_shape = -1, deck_badge = -1;

static void render_deck(void) {
  int32_t shape = R.show_deck_spot | R.show_deck_pile << 1 | R.show_flipped_slot << 2 |
                  R.show_flipped_card << 3 | R.show_trump_icon << 4 | (V->flipped.suit & 7) << 5 |
                  (V->flipped.value & 31) << 8 | (V->power_suit & 7) << 13;
  if (shape != deck_shape) { /* what the area holds changed: the deck ran out, a new deal */
    deck_shape = shape;
    cd_list_forget(&deck_layers, &DECK_LAYERS);
    cd_region_hide(&deck_area);
    deck_badge_txt = deck_layers_box = 0;
    deck_badge = -1;
    cd_region_show(&deck_area, 1);
    cd_at(deck_area.parent, 0);
    if (R.show_deck_spot) {
      OPEN("div"); STYLE("position: relative;"); ATTR("data-location", "deck");
      OPEN("div"); STYLE("position: relative; width: 50px; height: 70px;");
      cd_name_open(deck_layers.parent = deck_layers_box = cd_id_new());
      deck_layers.owns_parent = 1;
      CLOSE();
      if (R.show_deck_pile) {
        OPEN("p"); STYLE(BADGE);
        cd_text_num((uint32_t)R.deck_badge);
        cd_name(deck_badge_txt = cd_id_new());
        deck_badge = R.deck_badge;
        CLOSE();
      }
      CLOSE();
    }
    if (R.show_flipped_slot) {
      OPEN("div"); ATTR("data-location", "flipped");
      cd_attr(S_STYLE, R.show_deck_spot ? ZS("margin-top: -30px; width: 50px; height: 70px; z-index: 0;")
                                        : ZS("margin-top: 0px; width: 50px; height: 70px; z-index: 0;"));
      if (R.show_flipped_card) {
        face_open(V->flipped, 0);
        cd_attr(S_STYLE, face_style(V->flipped, FACE_STILL, 0, 0, 0, 0));
        CLOSE();
      }
      CLOSE();
    }
    if (R.show_trump_icon) {
      OPEN("div"); STYLE("display: flex; align-items: center; justify-content: center;");
      OPEN("span");
      cd_attr(S_STYLE, CARD_SUIT_FACE[V->power_suit].red ? ZS("font-size: 64px; line-height: 1; color: #B32929;")
                                                        : ZS("font-size: 64px; line-height: 1; color: #0A0A0A;"));
      cd_text(S_SUIT[V->power_suit]);
      CLOSE(); CLOSE();
    }
    cd_region_end(&deck_area);
  }
  if (deck_layers_box) layers_update(&deck_layers, (uint32_t)R.deck_pile, &DECK_LAYERS);
  if (deck_badge_txt && deck_badge != R.deck_badge) cd_set_text_num(deck_badge_txt, (uint32_t)(deck_badge = R.deck_badge));
}

/* ---- the discard pile (DiscardPile.tsx) ---------------------------------- */

static cd_id discard_txt;
static cd_region discard; /* the pile and its count, in the discard spot */
static cd_list discard_layers;
static int32_t discard_n = -1;

static void render_discard(void) {
  int32_t n = V->discard_pile_length;
  if (n == discard_n) return;
  if (n == 0 || discard_n <= 0) { /* the pile appears or goes */
    cd_list_forget(&discard_layers, &DISCARD_LAYERS);
    cd_region_hide(&discard);
    if (cd_region_show(&discard, n > 0)) {
      cd_at(discard.parent, 0);
      OPEN("div"); STYLE("position: relative; width: 50px; height: 70px;");
      cd_name_open(discard_layers.parent = cd_id_new());
      discard_layers.owns_parent = 1;
      CLOSE();
      OPEN("p"); STYLE(BADGE); cd_text_num((uint32_t)n); cd_name(discard_txt = cd_id_new()); CLOSE();
      cd_region_end(&discard);
    }
  } else {
    cd_set_text_num(discard_txt, (uint32_t)n);
  }
  discard_n = n;
  if (discard.shown) layers_update(&discard_layers, (uint32_t)n, &DISCARD_LAYERS);
}

static cd_str index_str(uint32_t i) {
  char b[12];
  return cd_tmp(b, cd_fmt(b, (int32_t)i, 0, CD_UNIT_NONE));
}

/* ---- the battles (TableBattles.tsx) -------------------------------------- */

enum { TAG_CARD = 0x100, TAG_PILL = 0x200, TAG_BATTLE = 0x400, TAG_TABLE = 0x800, TAG_AGAIN = 0x1000 };

static cd_list battles;
static cd_id battle_attack[52];  /* the attack face of each battle, by attack card id */
static cd_id battle_defense[52]; /* its cover's face, 0 while uncovered */

/* A battle item's data: its cover's card id + 1, 0 while uncovered. */
static uint32_t cover_data(const Battle *b) { return b->defense.suit >= 0 ? (uint32_t)card_to_id(b->defense) + 1 : 0; }

static void defense_add(int aid, Card d) {
  face_open(d, 0);
  cd_attr(S_STYLE, face_style(d, FACE_DEFENSE, 0, 0, 0, 0));
  cd_name_open(battle_defense[aid] = cd_id_new());
  data_card(d);
  CLOSE();
}

static cd_id battle_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  const Battle *b = &V->battles[i];
  int covered = b->defense.suit >= 0;
  int aid = card_to_id(b->attack);
  cd_id cell = cd_id_new();
  OPEN("div");
  cd_listen(cell, CD_EV_CLICK);
  cd_tag_set(cell, TAG_BATTLE | (uint32_t)aid);
  STYLE("display: flex; flex-direction: column; align-items: center; position: relative; width: 60px; "
        "height: 80px; margin: 5px; justify-content: center;");
  ATTR("data-location", "table");
  face_open(b->attack, 0);
  cd_attr(S_STYLE, face_style(b->attack, FACE_ATTACK, 0, 0, 0, covered));
  cd_name_open(battle_attack[aid] = cd_id_new());
  cd_attr(ZS("data-battle-index"), index_str(i));
  data_card(b->attack);
  CLOSE();
  if (covered) defense_add(aid, b->defense);
  CLOSE();
  *data = cover_data(b);
  return cell;
}

/* A battle whose cover changed: covered (the attack tilts back), or, when the board is replaced (a
 * load), uncovered or covered by another card. */
static void battle_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  const Battle *b = &V->battles[i];
  uint32_t want = cover_data(b);
  if (want == it->data) return;
  int aid = card_to_id(b->attack);
  cd_id att = battle_attack[aid];
  if (it->data) { cd_remove(battle_defense[aid]); battle_defense[aid] = 0; }
  it->data = want;
  cd_set_style(att, ZS("transform"), want ? ZS("translateX(-50%) rotate(-" COVER_TILT ")") : ZS("translateX(-50%)"));
  cd_set_style(att, ZS("z-index"), want ? ZS("1") : ZS("2"));
  if (want) { cd_at(it->node, 0); defense_add(aid, b->defense); }
}

static void battle_destroy(void *ctx, cd_item *it) {
  (void)ctx;
  int aid = (int)it->key - 1;
  cd_id_free(battle_attack[aid]);
  battle_attack[aid] = 0;
  if (battle_defense[aid]) { cd_id_free(battle_defense[aid]); battle_defense[aid] = 0; }
}

static const cd_list_fns BATTLE_FNS = {0, battle_create, battle_patch, battle_destroy};

static void render_table(void) {
  for (int i = 0; i < V->num_battles; i++) keys[i] = (uint32_t)card_to_id(V->battles[i].attack) + 1;
  cd_list_update(&battles, keys, (uint32_t)V->num_battles, &BATTLE_FNS);
}

/* ---- my hand (ActionButtons.tsx CardDiv, handShape.tsx) ------------------- */

static const HandMetrics WEB_HAND = {50, 20, 70, 2, 6, 0, 34};
static HandLayout HL;
static double lift, act_h = -1, pills_b = -1, plane_h = -1; /* lift, and the three lengths as drawn */
static cd_id action_area, pills_box, hand_plane;
static cd_list hand, pills;
/* What each hand card shows now, by card id: its box, its bottom, selected, its index. */
static struct { double x, w, h, bottom; int8_t sel, index; } shown[52];

static int selected(int cid) {
  for (int i = 0; i < nsel; i++) if (sel[i] == cid) return 1;
  return 0;
}

static int thin(const HandRect *r) { return r->w < 40 && r->h > 60; }
static double card_bottom(const HandRect *r) { return HL.rows.height - r->y - r->h; }

static cd_id hand_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  Card c = V->my_hand[i];
  const HandRect *r = &HL.slot[i];
  int cid = card_to_id(c);
  cd_id id = cd_id_new();
  face_open(c, thin(r));
  cd_attr(S_STYLE, face_style(c, FACE_HAND, r, card_bottom(r), selected(cid), 0));
  shown[cid].x = r->x; shown[cid].w = r->w; shown[cid].h = r->h; shown[cid].bottom = card_bottom(r);
  shown[cid].sel = (int8_t)selected(cid);
  cd_listen(id, CD_EV_CLICK);
  cd_tag_set(id, TAG_CARD | (uint32_t)cid);
  cd_attr(ZS("data-card-index"), index_str(i));
  shown[cid].index = (int8_t)i;
  ATTR("data-location", "hand");
  cd_attr(ZS("data-player-id"), ZS("u-me-0000"));
  data_card(c);
  ATTR("draggable", "true");
  CLOSE();
  *data = (uint32_t)cid;
  return id;
}

/* A surviving card: only the properties that changed, one style property each. */
static void hand_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  int cid = (int)it->data;
  const HandRect *r = &HL.slot[i];
  double bottom = card_bottom(r);
  int8_t sl = (int8_t)selected(cid);
  if (r->x != shown[cid].x) cd_set_style_num(it->node, ZS("left"), shown[cid].x = r->x, CD_UNIT_PX);
  if (bottom != shown[cid].bottom) cd_set_style_num(it->node, ZS("bottom"), shown[cid].bottom = bottom, CD_UNIT_PX);
  if (r->w != shown[cid].w) cd_set_style_num(it->node, ZS("width"), shown[cid].w = r->w, CD_UNIT_PX);
  if (r->h != shown[cid].h) cd_set_style_num(it->node, ZS("height"), shown[cid].h = r->h, CD_UNIT_PX);
  if (sl != shown[cid].sel) cd_set_style(it->node, ZS("border-color"), (shown[cid].sel = sl) ? ZS("red") : ZS("black"));
  if (shown[cid].index != (int8_t)i) cd_set_attr(it->node, ZS("data-card-index"), index_str((uint32_t)(shown[cid].index = (int8_t)i)));
}

static void hand_destroy(void *ctx, cd_item *it) { (void)ctx; (void)it; }
static const cd_list_fns HAND_FNS = {0, hand_create, hand_patch, hand_destroy};

/* The pill column, top to bottom, as ActionButtons.tsx COLUMN. */
static const struct { uint32_t bit; const char *label; double seed; } PILL[] = {
  {PLAY_PILL_ATTACK, "Attack", 0.25}, {PLAY_PILL_COVER, "Cover", 0.45}, {PLAY_PILL_PASS, "Pass", 0.35},
  {PLAY_PILL_PICKUP, "Pickup", 0.15}, {PLAY_PILL_GOOD, "Good", 0.85},
};

/* The seeds of the wood surfaces that are not pills (ActionButtons, the end screen, the back button). */
#define SEED_BACK 0.2
#define SEED_RESULTS 0.42
#define SEED_AGAIN 0.6

/* TexturedSurface.tsx getTextureStyle's background position for a seed (clearOfTileEdge with the
 * 260 x 60 plank span). */
enum { WOOD_W = 1920, WOOD_H = 1080 };
static void wood_offset(double seed, int32_t *x, int32_t *y) {
  *x = (int32_t)__builtin_floor(seed * WOOD_W);
  *y = (int32_t)(seed * 1000 - WOOD_H * __builtin_floor(seed * 1000 / WOOD_H));
  int32_t sx = (WOOD_W - *x % WOOD_W) % WOOD_W, sy = (WOOD_H - *y % WOOD_H) % WOOD_H;
  if (sx > WOOD_W - 260) *x = 260;
  if (sy > WOOD_H - 60) *y = 60;
}

/* TexturedSurface.tsx getTextureStyle: the wood once it is made (--tex-wood, or --tex-wood-end on
 * the end screen), the stripes before. */
static cd_str wood_style(double seed, int end) {
  static SB s;
  int32_t x, y;
  wood_offset(seed, &x, &y);
  sb_reset(&s);
  sb_s(&s, "background-color: rgb(195, 63, 8); background-image: var(");
  sb_s(&s, end ? "--tex-wood-end" : "--tex-wood");
  sb_s(&s, ", repeating-linear-gradient(90deg, transparent, "
           "transparent 1px, rgba(0, 0, 0, 0.1) 1px, rgba(0, 0, 0, 0.1) 2px)); background-size: 1920px 1080px; "
           "background-position: ");
  sb_i(&s, x); sb_s(&s, "px "); sb_i(&s, y);
  sb_s(&s, "px; background-repeat: repeat;");
  return sb_tmp(&s);
}

static uint32_t pill_order[5];
static cd_id pill_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  int p = (int)pill_order[i];
  cd_id wrap = cd_id_new(), btn = cd_id_new();
  OPEN("div");
  cd_name_open(wrap);
  OPEN("button");
  cd_listen(btn, CD_EV_CLICK);
  cd_tag_set(btn, TAG_PILL | PILL[p].bit);
  CLASS("btn-action");
  cd_attr(S_STYLE, wood_style(PILL[p].seed, 0));
  cd_attr(ZS("data-pill"), cd_internz(PILL[p].label));
  OPEN("span"); CLASS("btn-action-text"); OPEN("span"); cd_text(cd_internz(PILL[p].label)); CLOSE(); CLOSE();
  CLOSE();
  CLOSE();
  *data = btn;
  return wrap;
}
static void pill_patch(void *ctx, uint32_t i, cd_item *it) { (void)ctx; (void)i; (void)it; }
static void pill_destroy(void *ctx, cd_item *it) { (void)ctx; cd_id_free(it->data); }
static const cd_list_fns PILL_FNS = {0, pill_create, pill_patch, pill_destroy};

/* What the selection means on this board: the kernel's answer. */
static int ask_play(int8_t target, ClientPlay *out) {
  ClientGesture g;
  __builtin_memset(&g, 0, sizeof g);
  g.n_cards = (int8_t)nsel;
  g.target = target;
  for (int i = 0; i < nsel; i++) g.cards[i] = card_of_id(sel[i]);
  const ClientPlayScratch s = {&MOVES, MENU, (int)sizeof MENU};
  return client_play(&CT, V, &g, &s, out);
}

/* The kernel's pills for the selection (all = 1) or for none (all = 0). */
static uint32_t pills_for(int all) {
  if (V->status != GAME_STATUS_PLAYING || V->seats[ME].status != PLAYER_STATUS_IN) return 0;
  int n = nsel;
  if (!all) nsel = 0;
  ClientPlay p;
  uint32_t bits = ask_play(PLAY_TARGET_HAND, &p) == CLIENT_OK ? p.pills : 0;
  nsel = n;
  return bits;
}

/* The pills shown (ActionButtons.tsx): the kernel's for the selection, less the pressed ones. A
 * press is dropped when its pill rises again, read with the empty selection's pills folded in,
 * so putting a card down and picking it up does not re-arm a press (React's `rearm`). */
static uint32_t current_pills(void) {
  uint32_t bits = pills_for(1), rearm = bits | pills_for(0);
  pressed &= ~(rearm & ~prev_rearm);
  prev_rearm = rearm;
  return bits & ~pressed;
}

static void render_hand(void) {
  cd_viewport_rec vp;
  cd_viewport(&vp);
  double width = (double)vp.w - 20; /* the plane: the window less cards.css's 10px gutters */
  hand_layout(&WEB_HAND, V->my_hand_count, width, &HL);
  HandLayout empty;
  hand_layout(&WEB_HAND, 0, width, &empty);
  lift = HL.rows.height - empty.rows.height;

  if (act_h < 0) { /* the first render writes the whole style, as React's first render does */
    static SB s;
    sb_reset(&s);
    sb_s(&s, "display: flex; flex-direction: column; position: absolute; bottom: max(10px, env(safe-area-inset-bottom)); "
             "left: 0px; right: 0px; justify-content: end; align-items: center; ");
    sb_px(&s, "height: ", 200 + lift);
    cd_set_attr(action_area, S_STYLE, sb_tmp(&s));
    sb_reset(&s);
    sb_s(&s, "position: absolute; ");
    sb_px(&s, "bottom: ", 90 + lift);
    sb_s(&s, "right: 20px; display: flex; flex-direction: column; gap: 5px; z-index: 999;");
    cd_set_attr(pills_box, S_STYLE, sb_tmp(&s));
    sb_reset(&s);
    sb_s(&s, "position: relative; width: 100%; ");
    sb_px(&s, "height: ", HL.rows.height);
    cd_set_attr(hand_plane, S_STYLE, sb_tmp(&s));
  } else {
    if (200 + lift != act_h) cd_set_style_num(action_area, ZS("height"), 200 + lift, CD_UNIT_PX);
    if (90 + lift != pills_b) cd_set_style_num(pills_box, ZS("bottom"), 90 + lift, CD_UNIT_PX);
    if (HL.rows.height != plane_h) cd_set_style_num(hand_plane, ZS("height"), HL.rows.height, CD_UNIT_PX);
  }
  act_h = 200 + lift;
  pills_b = 90 + lift;
  plane_h = HL.rows.height;

  /* A card that turns thin changes its layout, so its key does too. */
  for (int i = 0; i < V->my_hand_count; i++)
    keys[i] = (uint32_t)card_to_id(V->my_hand[i]) + 1 + (thin(&HL.slot[i]) ? 64u : 0u);
  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);

  uint32_t bits = current_pills(), n = 0;
  for (uint32_t p = 0; p < 5; p++)
    if (bits & PILL[p].bit) { pill_order[n] = p; keys[n++] = PILL[p].bit; }
  cd_list_update(&pills, keys, n, &PILL_FNS);
}

/* ---- the seats (PlayerRing.tsx, RoleCoin.tsx, RoleMark.tsx) --------------- */

enum { MARK_NONE, MARK_SHIELD, MARK_SWORD, MARK_LEAD, MARK_CHECK };
static const char *const MARK_NAME[] = {"", "shield", "sword", "leadSword", "check"};

typedef struct {
  cd_region seat, badge; /* the seat's whole ring entry, and its mini hand's count badge */
  cd_id role, hand, count_txt;
  cd_list minis;
  int mark, count;
  double lift; /* the lift the root's transform was drawn for */
} SeatUI;
static SeatUI seat_ui[MAX_PLAYERS];
static int n_seats;
static cd_id board, seats_anchor;

/* roleLedger.ts markWorn over the board on screen. */
static int mark_of(const TableView *v, int seat) {
  const ViewSeat *p = &v->seats[seat];
  if (p->status == PLAYER_STATUS_OUT || v->status != GAME_STATUS_PLAYING) return MARK_NONE;
  if ((v->good_mask >> seat) & 1) return MARK_CHECK;
  if (v->defender == seat) return MARK_SHIELD;
  if (v->num_battles > 0 || seat == v->first_attacker)
    return seat == v->first_attacker ? MARK_LEAD : MARK_SWORD;
  return MARK_NONE;
}

#define SWORD_PATH "M12 1.2 L13.6 5.5 L13.6 14.3 L18 14.3 L18 16.6 L13.1 16.6 L13.1 19.6 L10.9 19.6 L10.9 16.6 L6 16.6 L6 14.3 L10.4 14.3 L10.4 5.5 Z"

static void mark_emit(int mark) {
  OPEN("span");
  ATTR("role", "img");
  cd_attr(ZS("aria-label"), mark == MARK_SHIELD ? ZS("Defending") : mark == MARK_LEAD ? ZS("You attack first")
                            : mark == MARK_SWORD ? ZS("attacking") : ZS("said good"));
  STYLE("display: inline-flex; line-height: 0;");
  OPEN("svg");
  cd_str size = mark == MARK_SHIELD ? ZS("33") : mark == MARK_CHECK ? ZS("26") : ZS("40");
  cd_attr(ZS("width"), size); cd_attr(ZS("height"), size);
  ATTR("viewBox", "0 0 24 24"); ATTR("fill", "none"); ATTR("aria-hidden", "true"); ATTR("focusable", "false");
  if (mark == MARK_SHIELD) {
    OPEN("path");
    ATTR("d", "M12 1.5 Q15.5 5 21 5.5 Q21 15.5 12 22.5 Q3 15.5 3 5.5 Q8.5 5 12 1.5 Z");
    ATTR("fill", "#FFFFFF"); ATTR("stroke", "#101014"); ATTR("stroke-width", "1.6"); ATTR("stroke-linejoin", "round");
    CLOSE();
  } else if (mark == MARK_CHECK) {
    for (int k = 0; k < 2; k++) {
      OPEN("path");
      ATTR("d", "M4 12.5 L9.5 18.5 L20 5");
      cd_attr(ZS("stroke"), k ? ZS("#2E9E4F") : ZS("#101014"));
      cd_attr(ZS("stroke-width"), k ? ZS("3.4") : ZS("6.6"));
      ATTR("stroke-linecap", "round"); ATTR("stroke-linejoin", "round");
      CLOSE();
    }
  } else {
    cd_str fill = mark == MARK_LEAD ? ZS("#DCBABA") : ZS("#FFFFFF");
    OPEN("g"); ATTR("transform", "rotate(45 12 12)");
    OPEN("path"); ATTR("d", SWORD_PATH); cd_attr(ZS("fill"), fill);
    ATTR("stroke", "#101014"); ATTR("stroke-width", "1.6"); ATTR("stroke-linejoin", "round"); CLOSE();
    OPEN("circle"); ATTR("cx", "12"); ATTR("cy", "20.6"); ATTR("r", "2"); cd_attr(ZS("fill"), fill);
    ATTR("stroke", "#101014"); ATTR("stroke-width", "1.6"); CLOSE();
    CLOSE();
  }
  CLOSE();
  CLOSE();
}

static double mid_of(int n) { return (n - 1) / 2.0; }
static double mini_left(uint32_t i, int n) { return 50 + (i - mid_of(n)) * 10 - 12.5; }
static cd_str mini_style(uint32_t i, int n) {
  static SB s;
  sb_reset(&s);
  sb_s(&s, "box-sizing: border-box; background-color: var(--tex-fern-bg, rgb(180, 14, 9)); width: 25px; height: 35px; border-radius: 3px; "
           "border: 1px solid rgb(139, 0, 0); position: absolute; top: 0px; ");
  sb_px(&s, "left: ", mini_left(i, n));
  sb_s(&s, "z-index: "); sb_i(&s, i);
  sb_s(&s, "; box-shadow: 0 1px 2px rgba(0,0,0,0.3); background-image: var(--tex-fern); background-size: 100% 100%; "
           "background-repeat: no-repeat; overflow: hidden;");
  return sb_tmp(&s);
}

/* A mini card's data is its left as drawn, in 1/10000 px. */
static uint32_t left_q(double v) { return (uint32_t)(int32_t)(v * 10000); }
static cd_id mini_create(void *ctx, uint32_t i, uint32_t *data) {
  SeatUI *u = ctx;
  cd_id id = cd_id_new();
  OPEN("div"); cd_name_open(id); cd_attr(S_STYLE, mini_style(i, u->count)); CLOSE();
  *data = left_q(mini_left(i, u->count));
  return id;
}
static void mini_patch(void *ctx, uint32_t i, cd_item *it) {
  SeatUI *u = ctx;
  double left = mini_left(i, u->count);
  if (left_q(left) != it->data) { it->data = left_q(left); cd_set_style_num(it->node, ZS("left"), left, CD_UNIT_PX); }
}
static void mini_destroy(void *ctx, cd_item *it) { (void)ctx; (void)it; }

static int seat_view(int seat) { return (seat - V->my_seat + n_seats) % n_seats; }
/* My seat rides up with the hand's lift. */
static void sb_transform(SB *s, int seat) {
  if (seat_view(seat) == 0 && lift > 0) { sb_s(s, "translate(-50%, calc(-50% - "); sb_num(s, lift, CD_UNIT_PX); sb_s(s, "))"); }
  else sb_s(s, "translate(-50%, -50%)");
}
static cd_str seat_style(int seat) {
  static SB s;
  double rad = 2 * M_PI * seat_view(seat) / n_seats;
  sb_reset(&s);
  sb_s(&s, "position: absolute; top: "); sb_num(&s, cos(rad) * 35 + 50, CD_UNIT_PCT);
  sb_s(&s, "; left: "); sb_num(&s, -sin(rad) * 35 + 50, CD_UNIT_PCT);
  sb_s(&s, "; display: flex; flex-direction: column; align-items: center; width: 80px; height: 91px; transform: ");
  sb_transform(&s, seat);
  sb_s(&s, ";");
  return sb_tmp(&s);
}
static cd_str seat_transform(int seat) {
  static SB s;
  sb_reset(&s);
  sb_transform(&s, seat);
  return sb_tmp(&s);
}
static double seat_lift(int seat) { return seat_view(seat) == 0 && lift > 0 ? lift : 0; }

#define ROLE_ROW "height: 40px; width: 40px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; " \
  "transform: scaleX(1); will-change: transform;"
#define NAME_ROW "margin: 0px; font-size: 12px; color: var(--color-text-primary); text-shadow: var(--text-shadow-label); " \
  "white-space: nowrap; text-align: center; height: 16px; line-height: 16px; flex-shrink: 0; display: flex; " \
  "align-items: center; justify-content: center; position: relative;"
#define COUNT_BOX "position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: var(--color-text-primary); " \
  "font-size: 10px; font-weight: bold; z-index: 100; pointer-events: none; text-shadow: 1px 1px 2px rgba(0,0,0,0.8);"

static const cd_list_fns *mini_fns(SeatUI *u) {
  static cd_list_fns f[MAX_PLAYERS];
  int k = (int)(u - seat_ui);
  f[k] = (cd_list_fns){u, mini_create, mini_patch, mini_destroy};
  return &f[k];
}

static void seats_build(void) {
  for (int s = 0; s < n_seats; s++) { /* a new table: the old ring goes */
    SeatUI *u = &seat_ui[s];
    cd_list_forget(&u->minis, mini_fns(u));
    cd_region_hide(&u->badge);
    cd_region_hide(&u->seat);
  }
  n_seats = V->num_players;
  for (int s = 0; s < n_seats; s++) {
    SeatUI *u = &seat_ui[s];
    u->mark = -1;
    u->count = 0;
    u->lift = seat_lift(s);
    cd_region_show(&u->seat, 1);
    cd_at(board, seats_anchor);
    OPEN("div");
    cd_name_open(u->seat.root);
    cd_attr(S_STYLE, seat_style(s));
    OPEN("div");
    cd_name_open(u->role = cd_id_new());
    cd_attr(ZS("data-role-seat"), index_str((uint32_t)s));
    ATTR("data-role-mark", "");
    STYLE(ROLE_ROW);
    CLOSE();
    OPEN("div"); STYLE(NAME_ROW); cd_text(cd_internz(SEAT_NAME[s])); CLOSE();
    OPEN("div");
    cd_name_open(u->hand = cd_id_new());
    STYLE("display: flex; flex-direction: row; align-items: center; justify-content: center; position: relative; "
          "height: 35px; width: 100px; flex-shrink: 0;");
    if (s != V->my_seat) { ATTR("data-location", "hand"); cd_attr(ZS("data-player-id"), cd_internz(SEAT_ID[s])); }
    CLOSE();
    CLOSE();
    cd_region_end(&u->seat);
    u->minis.parent = u->hand;
  }
}

static void render_seats(void) {
  if (n_seats != V->num_players) seats_build();
  for (int s = 0; s < n_seats; s++) {
    SeatUI *u = &seat_ui[s];
    if (seat_lift(s) != u->lift) { u->lift = seat_lift(s); cd_set_style(u->seat.root, ZS("transform"), seat_transform(s)); }
    int mark = mark_of(V, s);
    if (mark != u->mark) {
      u->mark = mark;
      cd_clear(u->role);
      cd_set_attr(u->role, ZS("data-role-mark"), cd_internz(MARK_NAME[mark]));
      if (mark != MARK_NONE) { cd_at(u->role, 0); mark_emit(mark); }
    }
    int n = s == V->my_seat ? V->my_hand_count : V->seats[s].hand_count;
    if (n != u->count) {
      u->count = n;
      for (int i = 0; i < n; i++) keys[i] = (uint32_t)i + 1;
      cd_list_update(&u->minis, keys, (uint32_t)n, mini_fns(u)); /* new cards go before the count */
      if (u->badge.shown && n > 0) cd_set_text_num(u->count_txt, (uint32_t)n);
      if (cd_region_show(&u->badge, n > 0)) {
        cd_at(u->hand, 0);
        OPEN("div"); cd_name_open(u->badge.root); STYLE(COUNT_BOX);
        cd_text_num((uint32_t)n); cd_name(u->count_txt = cd_id_new());
        CLOSE();
        cd_region_end(&u->badge);
      }
      u->minis.end = u->badge.shown ? u->badge.root : 0;
    }
  }
}

/* ---- the end of the game -------------------------------------------------- */

static cd_region end_screen; /* root mode: one element over the board */

/* A row of WinScreen.tsx: RankIcon (the default theme's emoji: a medal by place, the fool's card
 * for the last), the name, and "(You)" after mine. */
static void result_row(int place, int total, int seat) {
  static const char *const MEDAL[] = {"\xF0\x9F\xA5\x87", "\xF0\x9F\xA5\x88", "\xF0\x9F\xA5\x89"};
  OPEN("div"); cd_class(seat == ME ? ZS("result-card result-card--current-user") : ZS("result-card "));
  OPEN("div"); CLASS("flex items-center gap-md flex-1 min-w-0");
  OPEN("div"); CLASS("result-card__rank"); OPEN("span"); CLASS("");
  cd_text(cd_internz(place == total ? "\xF0\x9F\x83\x8F" : MEDAL[place - 1])); /* the table has two seats: places 1 to 2 */
  CLOSE(); CLOSE();
  OPEN("div"); CLASS("flex items-baseline gap-sm min-w-0");
  OPEN("span"); cd_class(seat == ME ? ZS("result-card__name result-card__name--current") : ZS("result-card__name "));
  cd_text(cd_internz(SEAT_NAME[seat])); CLOSE();
  if (seat == ME) { OPEN("span"); CLASS("result-card__you"); TEXT("("); OPEN("span"); TEXT("You"); CLOSE(); TEXT(")"); CLOSE(); }
  CLOSE(); CLOSE(); CLOSE();
}

static void render_end(void) {
  if (!cd_region_show(&end_screen, V->status == GAME_STATUS_GAME_OVER)) return;
  cd_at(board, 0);
  OPEN("div");
  cd_name_open(end_screen.root);
  ATTR("data-end", "true");
  STYLE("position: absolute; inset: 0px; display: flex; flex-direction: column; align-items: center; "
        "justify-content: center; gap: 16px; z-index: 2000; background-color: rgba(0, 0, 0, 0.35);");
  OPEN("h1"); CLASS("win-screen__title"); TEXT("done - Two of us"); CLOSE(); /* "done - " and the table's title */
  OPEN("div"); CLASS("win-screen__results"); STYLE("width: min(360px, 90vw);");
  OPEN("div"); CLASS("bg-wood"); cd_attr(S_STYLE, wood_style(SEED_RESULTS, 1)); CLOSE();
  /* The finish order is the kernel's (anim_finish_rows: everyone out, first out first, then the fool). */
  AnimFinishRow rows[MAX_PLAYERS + 1];
  int n = anim_finish_rows((const unsigned char *)V->elimination, V->num_eliminated, V->fool, V->num_players, V->my_seat, rows, MAX_PLAYERS + 1);
  for (int place = 1; place <= n; place++)
    for (int i = 0; i < n; i++) if (rows[i].place == place) result_row(place, n, rows[i].seat);
  CLOSE();
  cd_id again = cd_id_new();
  OPEN("button"); cd_listen(again, CD_EV_CLICK); cd_tag_set(again, TAG_AGAIN);
  CLASS("btn-wood btn-wood--lg"); cd_attr(S_STYLE, wood_style(SEED_AGAIN, 1));
  OPEN("span"); CLASS("btn-wood-text"); OPEN("span"); TEXT("Play again"); CLOSE(); CLOSE();
  CLOSE();
  CLOSE();
  cd_region_end(&end_screen); /* the button's id goes with the screen */
}

/* ---- one render ----------------------------------------------------------- */

static cd_id container, bg_wool;
static cd_str turn_shown;

static void render(void) {
  if (client_adopt_board(&CT, &G, ME) != CLIENT_OK) __builtin_trap();
  V = &CT.view;
  client_view_rules(V, 0, 0, 0, &R);
  render_deck();
  render_discard();
  render_hand();
  render_table();
  render_seats();
  render_end();
  cd_str turn = V->status == GAME_STATUS_GAME_OVER ? ZS("over") : bot_pending ? ZS("bot") : ZS("human");
  if (turn != turn_shown) cd_set_attr(container, ZS("data-turn"), turn_shown = turn);
}

/* ---- moves ---------------------------------------------------------------- */

enum { TIMER_TEX = 2, TIMER_BOT_GO = 3, TIMER_BOT_BASE = 16 }; /* the bot's: TIMER_BOT_BASE + its generation */
enum { RAF_SLICE = 0, RAF_PAINTED = 1, RAF_PAINTED2 = 2 };
/* Measured (tools/timing.mjs, BLANK=1, CPU x4): starting to make textures on a timer right after
 * the first frame, cdom.start resolved at 165 ms against 60 without textures; two frames and 50 ms
 * later, at 60. The store is asked as the table starts: a texture found there costs a url, not a
 * slice of work. */
#define TEX_START_MS 50
static void textures_begin(void);
static void tex_go(void);

/* ---- the game across loads (README, Reload) --------------------------------
 * After every operation the game is made canonical (the board a load of its blob holds) and its
 * blob is kept in cdom's store; a load asks the store for it as the table starts, and adopts it
 * when it reads as a game this table plays. The selection and the pressed pills are not kept, as
 * React keeps neither. The blob's clock is left zero: it would be this page's time, and a load
 * shows the board anew (shown_ms below starts at the adopt). */
#define GAME_KEY "foolish-game-v1"
enum { STORE_GAME = 100 }; /* the store's token for the game (the textures' are 0 to NTEX - 1) */
static unsigned char game_blob[GAME_KEEP_CAP];
static int game_touched;   /* an operation was committed on this load: the store's game is older */
static double game_asked, game_answered;
static int game_adopted;

static void game_save(void) {
  int len = game_keep_put(&G, game_blob);
  if (len > 0) cd_store_put(GAME_KEY, sizeof GAME_KEY - 1, game_blob, (uint32_t)len);
}

/* ---- the bot's pace (bot_drive.h bot_wait_ms) -------------------------------
 * The kernel says how long the bot waits: until the last operation shown has finished playing,
 * then BOT_PACE_MS_WITH_HUMANS (3000 ms) with a human in the game. The clock is the table's
 * BoardClock in this page's time: shown_ms is when the last operation (mine, the bot's, a deal,
 * a load) was shown, and settles_ms when it finishes playing, which with no animations yet is
 * the same instant (pass 3 adds the animation stream's end, anim_stream_ms). A timer fires when
 * the wait is over and asks again, so a move of mine in between moves the bot's turn back, as
 * the server's loop does. A timer cannot be cancelled: each schedule has its own generation, and
 * a timer of an older one does nothing. */
static double shown_ms;
static uint32_t bot_gen; /* the generation of the live bot timer */
static int last_wait;    /* the kernel's last answer, for the test */
#ifdef TABLE_DEBUG
static int dbg_hold, dbg_due, dbg_fast; /* the test build: hold a due step; fast: 30 ms waits */
#endif

static double settles_ms(void) { return shown_ms; }

/* The board was shown now (a clock of 0 is a board never shown). */
static void mark_shown(void) {
  double t = cd_now();
#ifdef TABLE_DEBUG
  if (dbg_fast) t -= BOT_PACE_MS_WITH_HUMANS - 30; /* fast: as if shown long enough ago that 30 ms are left */
#endif
  shown_ms = t >= 1 ? t : 1;
}

static uint32_t bot_wait(void) {
  int64_t now = (int64_t)cd_now();
  last_wait = bot_wait_ms(&G, game_human_mask(&G), (int64_t)shown_ms, (int64_t)settles_ms(), now);
  return (uint32_t)last_wait;
}

static int bot_may_act(void) { return G.status == GAME_STATUS_PLAYING && !my_turn_owed(); }

/* After every change: the bot's timer runs exactly while the bot may act. */
static void bot_check(void) {
  if (bot_may_act() == bot_pending) return;
  bot_gen++;
  bot_pending = bot_may_act();
  if (bot_pending) cd_timer_start(bot_wait(), TIMER_BOT_BASE + bot_gen);
}

/* After every operation on the game: canonical, kept, shown now. */
static void committed(void) {
  game_keep_canonical(&G);
  game_save();
  game_touched = 1;
  mark_shown();
}

/* One bot step: whatever the bot does up to its next visible move. */
static uint32_t bot_steps; /* how many bot steps this load has run */
static void bot_step(void) {
  BotDriveOut out;
  bot_steps++;
  bot_drive(&G, game_human_mask(&G), BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
  committed();
  bot_pending = 0;
  bot_gen++;
  bot_check();
  render();
}

/* The bot's timer: wait on if the kernel says so (a move came in between), else step. */
static void bot_due(uint32_t gen) {
  if (gen != bot_gen || !bot_pending) return;
  uint32_t w = bot_wait();
  if (w > 0) { cd_timer_start(w, TIMER_BOT_BASE + bot_gen); return; }
#ifdef TABLE_DEBUG
  if (dbg_hold) { dbg_due = 1; return; }
#endif
  bot_step();
}

static int play(int kind, const Card *cards, int n, const Card *attacks) {
  ClientMove m;
  __builtin_memset(&m, 0, sizeof m);
  m.kind = (int8_t)kind;
  m.n_cards = (uint8_t)n;
  m.n_attacks = kind == AWIRE_COVER ? (uint8_t)n : 0;
  for (int i = 0; i < n; i++) {
    m.cards[i] = cards[i];
    if (attacks) m.attack_cards[i] = attacks[i];
  }
  uint8_t wire[2 + 2 * AWIRE_MAX_CARDS];
  int len = client_move_wire(&m, wire, sizeof wire);
  AwireAction a;
  if (len <= 0 || !awire_decode(wire, len, &a)) return 0;
  if (!awire_apply(&G, ME, &a)) { last_reject = engine_last_reject; return 0; }
  last_reject = 0;
  committed();
  return 1;
}

/* A selection dropped on `target`, the table (an attack or a transfer) or a battle (a cover):
 * played where the kernel says it goes. A drop that is a move spends the selection, as React's
 * drag does (DragContext.tsx endCardDrag, moveType >= 0); one that is not leaves it. */
static void drop_selection(int8_t target) {
  ClientPlay p;
  if (!nsel || ask_play(target, &p) != CLIENT_OK || p.move_type < 0) return;
  nsel = 0;
  play(p.move_type, p.cards, p.n_cards, p.move_type == MOVE_COVER ? p.attack_cards : 0);
}

/* A pill (ActionButtons.tsx handle*Click): it is pressed (hidden) until the kernel's pill falls
 * and rises again (render_hand), or the move is refused. Attack, Pass and Cover spend the
 * selection when pressed, refused or not; Pickup and Good leave it. Cover aims itself
 * (client_play with the Cover button), and does nothing when the kernel has no cover. */
static void on_pill(uint32_t bit) {
  Card cards[MAX_MOVE_CARDS];
  int n = nsel, ok = 0;
  for (int i = 0; i < n; i++) cards[i] = card_of_id(sel[i]);
  if (bit == PLAY_PILL_COVER) {
    ClientPlay p;
    if (!nsel || ask_play(CLIENT_PLAY_COVER_BUTTON, &p) != CLIENT_OK || p.move_type != MOVE_COVER) return;
    pressed |= bit;
    nsel = 0;
    ok = play(AWIRE_COVER, p.cards, p.n_cards, p.attack_cards);
  } else {
    pressed |= bit;
    if (bit == PLAY_PILL_ATTACK || bit == PLAY_PILL_PASS) nsel = 0;
    if (bit == PLAY_PILL_ATTACK) ok = play(AWIRE_ATTACK, cards, n, 0);
    else if (bit == PLAY_PILL_PASS) ok = play(AWIRE_PASS, cards, n, 0);
    else if (bit == PLAY_PILL_PICKUP) ok = play(AWIRE_PICKUP, 0, 0, 0);
    else if (bit == PLAY_PILL_GOOD) ok = play(AWIRE_GOOD, 0, 0, 0);
  }
  if (!ok) pressed &= ~bit;
}

static void toggle(int cid) {
  for (int i = 0; i < nsel; i++)
    if (sel[i] == cid) { sel[i] = sel[--nsel]; return; }
  if (nsel < MAX_MOVE_CARDS) sel[nsel++] = (uint8_t)cid;
}

/* A game on the table that was not played to it here (a deal, a load): nothing of the last one's
 * selection or presses carries over. */
static void table_reset(void) {
  nsel = 0;
  pressed = 0;
  prev_rearm = 0;
}

static void new_game(void) {
  table_reset();
  deal_new();
  committed();
  bot_check();
  render();
}

/* The store's answer to the start's ask: the game left on an earlier load, adopted unless a move
 * has already been made on this one. */
static void game_stored(const uint8_t *p, int32_t len) {
  game_answered = cd_now();
  if (len <= 0 || game_touched || !game_keep_load(&G, p, len)) return;
  game_adopted = 1;
  table_reset();
  mark_shown();
  bot_pending = 0;
  bot_gen++;
  bot_check();
  render();
}

void cd_on_event(const cd_event_rec *e) {
  if (e->type == CD_EV_RESIZE) { render(); return; } /* the hand is laid out for the new width */
  if (e->type != CD_EV_CLICK) return;
  uint32_t tag = cd_tag((cd_id)e->id);
  if (tag & TAG_AGAIN) { new_game(); return; }
  if (V->status != GAME_STATUS_PLAYING) return;
  if (tag & TAG_CARD) toggle((int)(tag & 0xff));
  else if (tag & TAG_PILL) on_pill(tag & 0xff);
  else if (tag & TAG_BATTLE) {
    int aid = (int)(tag & 0xff), b = -1;
    for (int i = 0; i < V->num_battles; i++) if (card_to_id(V->battles[i].attack) == aid) b = i;
    if (b >= 0) drop_selection((int8_t)b);
  } else if (tag & TAG_TABLE) drop_selection(PLAY_TARGET_TABLE);
  bot_check();
  render();
}

void cd_on_timer(uint32_t token) {
  if (token >= TIMER_BOT_BASE) bot_due(token - TIMER_BOT_BASE);
#ifdef TABLE_DEBUG
  if (token == TIMER_BOT_GO) { bot_pending = 1; bot_step(); }
#endif
  if (token == TIMER_TEX) tex_go();
}

/* For the tests: the engine's reason for the last refused move (0 for none), and the string
 * table's size (the same strings are held in C's pool and in the shim). */
EXPORT("app_last_reject") int app_last_reject(void) { return last_reject; }
EXPORT("app_str_count") uint32_t app_str_count(void) { return cd_str_count(); }
EXPORT("app_str_bytes") uint32_t app_str_bytes(void) { return cd_str_bytes(); }
EXPORT("app_ids_live") uint32_t app_ids_live(void) { return cd_ids_live(); }
/* The kernel's last answer to how long the bot waits (bot_wait_ms), in ms. */
EXPORT("app_bot_wait") int32_t app_bot_wait(void) { return last_wait; }
EXPORT("app_bot_steps") uint32_t app_bot_steps(void) { return bot_steps; }
/* The game's store: when it was asked (which 0) and answered (1), ms after navigation; 2: 1 when
 * the stored game was adopted. */
EXPORT("app_game_store") double app_game_store(int32_t which) { return which == 0 ? game_asked : which == 1 ? game_answered : game_adopted; }

#ifdef TABLE_DEBUG
/* ---- the test build's window on the kernel (out/table_debug.wasm, ?debug) ----------------
 * What the table should show now, worked out afresh from the kernel and not from what was drawn:
 * the game round-tripped through its blob (the board a server's load of the row would play on),
 * read for the viewer by a ClientTable of its own, as lines of text the test compares with the
 * DOM read the same way (tools/consist.mjs). The pills are the kernel's raw answer for the
 * selection and for none: the pressed-pill rule is the test's model of React's. */
static Game DG;
static ClientSlot DSLOT;
static ClientTable DCT;
static unsigned char DBLOB[2048];
static char dbg[4096];
static uint32_t dbg_n;
static void d_s(const char *t) { while (*t && dbg_n < sizeof dbg - 1) dbg[dbg_n++] = *t++; }
static void d_i(int32_t v) { char b[16]; uint32_t n = cd_fmt(b, v, 0, CD_UNIT_NONE); for (uint32_t i = 0; i < n; i++) dbg[dbg_n++] = b[i]; }
static void d_card(Card c) { d_i(c.suit); d_s("-"); d_i(c.value); }
static void d_face(Card c) { d_s(CARD_RANK_LABEL[c.value]); d_s(CARD_SUIT_FACE[c.suit].glyph); }
static uint32_t d_pills(const TableView *v, int with_sel) {
  if (v->status != GAME_STATUS_PLAYING || v->seats[ME].status != PLAYER_STATUS_IN) return 0;
  ClientGesture g;
  __builtin_memset(&g, 0, sizeof g);
  g.n_cards = with_sel ? (int8_t)nsel : 0;
  g.target = PLAY_TARGET_HAND;
  for (int i = 0; i < g.n_cards; i++) g.cards[i] = card_of_id(sel[i]);
  const ClientPlayScratch sc = {&MOVES, MENU, (int)sizeof MENU};
  ClientPlay p;
  return client_play(&DCT, v, &g, &sc, &p) == CLIENT_OK ? p.pills : 0;
}
static void d_pill_names(uint32_t bits) { for (int p = 0; p < 5; p++) if (bits & PILL[p].bit) { d_s(" "); d_s(PILL[p].label); } }

EXPORT("app_debug_state") uint32_t app_debug_state(void) {
  static int inited;
  if (!inited) { client_init(&DCT, &DSLOT); inited = 1; }
  dbg_n = 0;
  int len = game_keep_put(&G, DBLOB);
  if (len <= 0 || !game_keep_load(&DG, DBLOB, len)) { d_s("error the game does not round-trip\n"); dbg[dbg_n] = 0; return (uint32_t)(uintptr_t)dbg; }
  if (client_adopt_board(&DCT, &DG, ME) != CLIENT_OK) __builtin_trap();
  const TableView *v = &DCT.view;
  ViewRules r;
  client_view_rules(v, 0, 0, 0, &r);
  d_s("status "); d_s(v->status == GAME_STATUS_GAME_OVER ? "over" : "playing");
  d_s("\nhand"); for (int i = 0; i < v->my_hand_count; i++) { d_s(" "); d_card(v->my_hand[i]); }
  { /* where hand_layout puts each card at this width: left, bottom, width, height (CSS px) */
    cd_viewport_rec vp;
    cd_viewport(&vp);
    static HandLayout dl;
    hand_layout(&WEB_HAND, v->my_hand_count, (double)vp.w - 20, &dl);
    d_s("\nslots");
    for (int i = 0; i < v->my_hand_count; i++) {
      const HandRect *r = &dl.slot[i];
      char b[64];
      d_s(" ");
      uint32_t n = cd_fmt_len(b, r->x, CD_UNIT_NONE); b[n++] = ','; n += cd_fmt_len(b + n, dl.rows.height - r->y - r->h, CD_UNIT_NONE);
      b[n++] = ','; n += cd_fmt_len(b + n, r->w, CD_UNIT_NONE); b[n++] = ','; n += cd_fmt_len(b + n, r->h, CD_UNIT_NONE); b[n] = 0;
      d_s(b);
    }
  }
  d_s("\nselected"); for (int i = 0; i < v->my_hand_count; i++) if (selected(card_to_id(v->my_hand[i]))) { d_s(" "); d_card(v->my_hand[i]); }
  d_s("\nsel"); for (int i = 0; i < nsel; i++) { d_s(" "); d_card(card_of_id(sel[i])); }
  d_s("\nkernel_pills"); d_pill_names(d_pills(v, 1));
  d_s("\nkernel_pills_none"); d_pill_names(d_pills(v, 0));
  d_s("\nbattles"); for (int i = 0; i < v->num_battles; i++) { d_s(" "); d_card(v->battles[i].attack); d_s("/"); if (v->battles[i].defense.suit >= 0) d_card(v->battles[i].defense); else d_s("-"); }
  d_s("\ndeck"); if (r.show_deck_spot && r.show_deck_pile) { d_s(" badge "); d_i(r.deck_badge); d_s(" layers "); d_i(r.deck_pile); }
  if (r.show_flipped_slot) { d_s(" flipped "); if (r.show_flipped_card) d_face(v->flipped); else d_s("-"); }
  if (r.show_trump_icon) { d_s(" trump "); d_s(CARD_SUIT_FACE[v->power_suit].glyph); }
  d_s("\ndiscard"); if (v->discard_pile_length > 0) { d_s(" "); d_i(v->discard_pile_length); }
  for (int s = 0; s < v->num_players; s++) {
    d_s("\nseat "); d_i(s); d_s(" "); d_s(mark_of(v, s) ? MARK_NAME[mark_of(v, s)] : "none");
    d_s(" "); d_i(s == v->my_seat ? v->my_hand_count : v->seats[s].hand_count);
  }
  d_s("\nfool "); d_i(v->fool);
  d_s("\nelimination"); for (int i = 0; i < v->num_eliminated; i++) { d_s(" "); d_i(v->elimination[i]); }
  d_s("\nbot_eligible "); d_i((int32_t)bot_drive_eligible_mask(&DG, game_human_mask(&DG)));
  d_s("\n");
  dbg[dbg_n] = 0;
  return (uint32_t)(uintptr_t)dbg;
}
EXPORT("app_debug_len") uint32_t app_debug_len(void) { return dbg_n; }
/* Hold: a bot step that falls due waits for app_debug_release (the test reads the table between
 * every two steps); app_debug_due says one is waiting. */
EXPORT("app_debug_hold") void app_debug_hold(int32_t on) { dbg_hold = on; }
EXPORT("app_debug_due") int32_t app_debug_due(void) { return dbg_due; }
EXPORT("app_debug_release") void app_debug_release(void) { if (dbg_due) { dbg_due = 0; cd_timer_start(0, TIMER_BOT_GO); } }
/* The kernel's random streams (the draws and the bot's choices) and the next deal's, seeded: a
 * test that needs one line of play asks for the seed that plays it. */
EXPORT("app_debug_reseed") void app_debug_reseed(uint32_t seed) {
  game_set_seed(seed);
  random_strategy_set_seed(seed);
  rng = seed | 1;
}
/* Fast: the bot waits 30 ms after a move rather than the kernel's 3000 (the games the tests play out):
 * the clock is put back, the kernel's rule is not touched. */
EXPORT("app_debug_fast") void app_debug_fast(int32_t on) { dbg_fast = on; }
#endif
/* ---- the textures ----------------------------------------------------------
 * After the first paint (two frames, then a timer), in slices of about 8 ms on animation frames: the fern (the card backs),
 * the wood (the buttons), then the wool (the background). Each lands as a custom property on the
 * mount (cd_set_image), which the styles above read through var(). Each is only as much as the
 * page shows (README, Textures):
 * - the fern at FERN_SCALE times the deck's card back in device pixels (not the app's 1000 x 1400),
 *   with the app's 5,000,000 points scaled by the area, so it is as dense;
 * - the wood's 1920 x 1080 tile worked out only where a button shows it (WOOD_SURF);
 * - the wool's middle 1920 x 1080 of the app's 3840 x 2160 (and a margin), all its scale(2) ever
 *   shows: woven only there, put in a 3840 x 2160 image (WOOL_W).
 * One block (8 MiB class) holds each in turn, RGB where the picture is opaque (the wool's crop, the
 * wood's tile), freed at the end; none is taken when all three come from the store. */
/* The wool's crop: the middle 1920 x 1080 widened by 16 columns and 12 rows a side, on JPEG's 16 x 16 block
 * grid as the app's whole picture is, so its blocks are the same pixels and encode to the same
 * JPEG blocks; the margin keeps the blocks at its edge (and their chroma) out of sight. It is put
 * at its place in a 3840 x 2160 image (the rest black, never shown): the page then lays it out,
 * snaps it to pixels and resamples it exactly as it does the app's picture. */
enum { TEX_W = 3840, TEX_H = 2160, WOOL_W = 1920 + 32, WOOL_H = 1080 + 24, WOOL_X = (TEX_W - WOOL_W) / 2, WOOL_Y = (TEX_H - WOOL_H) / 2 };
_Static_assert(WOOL_X % 16 == 0 && WOOL_Y % 16 == 0, "the crop starts on the JPEG block grid");
enum { FERN_APP_W = 1000, FERN_APP_H = 1400, FERN_APP_POINTS = 5000000, BACK_W = 50, BACK_H = 70, FERN_SCALE = 4 };
#define TEX_SLICE_MS 8.0
/* Every surface the wood is laid on besides the pills (PILL): its seed and the most it spans
 * (border box, CSS px, with room to spare: measured, the back button is 44 x 44, a pill 60 x 40,
 * Play again 149 x 48, the results panel min(360px, 90vw) by 81 for two rows). The wood is made
 * only there, so the less, the sooner it lands. tools/test.mjs checks, after every click of
 * played-out games, that each surface on the page lies inside the rectangle made for it. */
static const struct { double seed; int32_t w, h; } WOOD_SURF[] = {{SEED_BACK, 56, 56}, {SEED_AGAIN, 200, 60}, {SEED_RESULTS, 360, 100}};
enum { PILL_SPAN_W = 76, PILL_SPAN_H = 52 };
enum { NPILL = sizeof PILL / sizeof *PILL, NSURF = sizeof WOOD_SURF / sizeof *WOOD_SURF };
enum { WOOD_MARGIN = 4 }; /* a border's width either side, and then some */
static struct {
  int stage, nrect, nrect_now; /* rect[0, nrect_now): the table's wood; the rest: the end screen's */
  uint8_t *block;
  float *col;
  Fern fern; Wood wood; Wool wool;
  int32_t fern_w, fern_h, landed, from_store[NTEX];
  TexRect rect[WOOD_MAX_WIN];
  double ms[NTEX], at[NTEX], asked[NTEX], answered[NTEX];
  uint32_t seed[NTEX];
  char key[NTEX][48];
  uint32_t klen[NTEX];
} tex;
static uint32_t tex_seed;

/* The tile rectangles a surface with this seed and span shows, split where one runs over the
 * tile's edge; added to out[*n]. */
static void wood_rects_add(TexRect *out, int *n, double seed, int32_t sw, int32_t sh) {
  int32_t x, y;
  wood_offset(seed, &x, &y);
  /* the element's padding box starts at tile pixel (tile - x % tile) % tile, its border box earlier */
  int32_t x0 = (WOOD_W - x % WOOD_W) % WOOD_W - WOOD_MARGIN, y0 = (WOOD_H - y % WOOD_H) % WOOD_H - WOOD_MARGIN;
  int32_t w = sw + 2 * WOOD_MARGIN, h = sh + 2 * WOOD_MARGIN;
  for (int32_t ys = y0; ys < y0 + h;) {
    int32_t ty = ((ys % WOOD_H) + WOOD_H) % WOOD_H, th = WOOD_H - ty < y0 + h - ys ? WOOD_H - ty : y0 + h - ys;
    for (int32_t xs = x0; xs < x0 + w;) {
      int32_t tx = ((xs % WOOD_W) + WOOD_W) % WOOD_W, tw = WOOD_W - tx < x0 + w - xs ? WOOD_W - tx : x0 + w - xs;
      if (*n == WOOD_MAX_WIN) __builtin_trap();
      out[(*n)++] = (TexRect){tx, ty, tw, th};
      xs += tw;
    }
    ys += th;
  }
}

/* The table's surfaces (the back button, the pills), then the end screen's (*now: where they start). */
static int wood_rects(TexRect *out, int *now) {
  int n = 0;
  wood_rects_add(out, &n, WOOD_SURF[0].seed, WOOD_SURF[0].w, WOOD_SURF[0].h);
  for (int k = 0; k < NPILL; k++) wood_rects_add(out, &n, PILL[k].seed, PILL_SPAN_W, PILL_SPAN_H);
  *now = n;
  for (int k = 1; k < NSURF; k++) wood_rects_add(out, &n, WOOD_SURF[k].seed, WOOD_SURF[k].w, WOOD_SURF[k].h);
  return n;
}

/* Texture stage begins from tex_seed: the fern's mulberry32 state is the seed; the wood's offsets
 * are the seed's first two draws and the wool's its third and fourth, whichever were made here. */
static void tex_start(int stage) {
  tex.stage = stage;
  uint8_t *b = tex.block;
  uint32_t rng = tex_seed;
  if (stage == TEX_FERN) {
    int32_t points = (int32_t)((double)FERN_APP_POINTS * tex.fern_w * tex.fern_h / (FERN_APP_W * FERN_APP_H));
    fern_begin(&tex.fern, b, b + tex.fern_w * tex.fern_h * 4, tex.fern_w, tex.fern_h, points, rng);
  }
  if (stage == TEX_WOOD || stage == TEX_WOOD_END) {
    double ox = tex_random(&rng) * 1000 - 500, oy = tex_random(&rng) * 1000 - 500;
    int from = stage == TEX_WOOD ? 0 : tex.nrect_now, to = stage == TEX_WOOD ? tex.nrect_now : tex.nrect;
    wood_begin(&tex.wood, tex.col, b, WOOD_W, WOOD_H, ox, oy, tex.rect + from, to - from);
  }
  if (stage == TEX_WOOL) {
    tex_random(&rng); tex_random(&rng);
    double ox = tex_random(&rng) * 1000 - 500, oy = tex_random(&rng) * 1000 - 500;
    wool_begin(&tex.wool, b, TEX_W, TEX_H, WOOL_X, WOOL_Y, WOOL_W, WOOL_H, ox, oy);
  }
}

/* ---- the textures across loads: the encoded image is kept in cdom's store (IndexedDB) under a
 * key that names the generator's version and everything the picture depends on (the fern's size,
 * the wood's rectangles), as the app keeps its own in TextureCacheDB; a later load sets the
 * stored bytes and generates nothing. The stored value is the image file. */
static const char *const TEX_PROP[NTEX] = {"--tex-fern", "--tex-wood", "--tex-wool", "--tex-wood-end"};
static const char *const TEX_MIME[NTEX] = {"image/png", "image/png", "image/jpeg", "image/png"};

static void key_add(char *k, uint32_t *n, const char *s) { while (*s && *n < 47) k[(*n)++] = *s++; }
static void key_num(char *k, uint32_t *n, uint32_t v) {
  char d[10]; int m = 0;
  do d[m++] = (char)('0' + v % 10); while (v /= 10);
  while (m && *n < 47) k[(*n)++] = d[--m];
}
static void tex_keys(void) {
  static const char *const NAME[NTEX] = {"foolish-tex-fern-v1-", "foolish-tex-wood-v1-", "foolish-tex-wool-v1-", "foolish-tex-wood-v1-"};
  for (int k = 0; k < NTEX; k++) { tex.klen[k] = 0; key_add(tex.key[k], &tex.klen[k], NAME[k]); }
  key_num(tex.key[0], &tex.klen[0], (uint32_t)tex.fern_w); key_add(tex.key[0], &tex.klen[0], "x");
  key_num(tex.key[0], &tex.klen[0], (uint32_t)tex.fern_h);
  for (int part = 0; part < 2; part++) { /* the woods, by FNV-1a over their rectangles */
    uint32_t h = 2166136261u;
    for (int k = part ? tex.nrect_now : 0; k < (part ? tex.nrect : tex.nrect_now); k++) {
      const int32_t v[4] = {tex.rect[k].x, tex.rect[k].y, tex.rect[k].w, tex.rect[k].h};
      for (int f = 0; f < 4; f++) for (int byte = 0; byte < 4; byte++) h = (h ^ ((uint32_t)v[f] >> (8 * byte) & 0xff)) * 16777619u;
    }
    int k = part ? TEX_WOOD_END : TEX_WOOD;
    key_num(tex.key[k], &tex.klen[k], h);
  }
  key_num(tex.key[2], &tex.klen[2], WOOL_W); key_add(tex.key[2], &tex.klen[2], "x"); key_num(tex.key[2], &tex.klen[2], WOOL_H);
}

/* A texture's image file lands: its url on the mount; with the fern, the backs turn black under it,
 * as CardBack's do. */
static void tex_land(int k, const uint8_t *p, uint32_t len) {
  cd_set_image_bytes(CD_MOUNT, cd_internz(TEX_PROP[k]), p, len, cd_internz(TEX_MIME[k]));
  if (k == TEX_FERN) cd_set_style(CD_MOUNT, ZS("--tex-fern-bg"), ZS("#000000"));
  tex.at[k] = cd_now();
  tex.landed++;
}

/* The order textures are made in, when the store has none: the fern (small, on the card backs),
 * the wool (the whole background), the table's wood, the end screen's. */
static const int TEX_ORDER[NTEX] = {TEX_FERN, TEX_WOOL, TEX_WOOD, TEX_WOOD_END};
enum { TEX_ASKED, TEX_MISSING, TEX_LANDED };
static int tex_state[NTEX], tex_may_make, tex_making;

/* All four are asked of the store at once, as the page starts: a get costs the page's thread
 * nothing while it waits, so a texture kept from an earlier load lands as soon as it is read. */
static void tex_ask(void) {
  for (int k = 0; k < NTEX; k++) {
    tex_state[k] = TEX_ASKED;
    tex.asked[k] = cd_now();
    cd_store_get((uint32_t)k, tex.key[k], tex.klen[k], CD_STORE_DEADLINE_MS);
  }
}

/* Makes the next texture the store did not have, in TEX_ORDER, once making is allowed (TEX_START_MS
 * after the first paint) and nothing else is being made; one still being read is waited for, so
 * the order holds. The block goes back when every texture has landed. */
static void tex_make_next(void) {
  if (tex.landed == NTEX && tex.block) {
    cd_free(tex.block); cd_free(tex.col); /* back to cdom's free lists (wasm memory does not shrink) */
    tex.block = 0; tex.col = 0;
  }
  if (!tex_may_make || tex_making) return;
  int k = -1;
  for (int i = 0; i < NTEX && k < 0; i++) {
    int t = TEX_ORDER[i];
    if (tex_state[t] == TEX_ASKED) return; /* not read yet: it may be in the store */
    if (tex_state[t] == TEX_MISSING) k = t;
  }
  if (k < 0) return;
  if (!tex.block) { /* the first texture to make: the room for all of them */
    int32_t most = 0;
    for (int r = 0; r < tex.nrect; r++) if (tex.rect[r].w * tex.rect[r].h > most) most = tex.rect[r].w * tex.rect[r].h;
    uint32_t fern = (uint32_t)(tex.fern_w * tex.fern_h * 8), wool = WOOL_W * WOOL_H * 3, wood = WOOD_W * WOOD_H * 3;
    uint32_t big = wool > wood ? wool : wood;
    tex.block = cd_alloc(big > fern ? big : fern); /* 6,465,024 bytes (the wool, RGB): one 8 MiB size class */
    tex.col = cd_alloc((uint32_t)most * 12);       /* the wood's float colours, one rectangle at a time */
  }
  tex_making = 1;
  tex.seed[k] = tex_seed;
  tex_start(k);
  cd_raf_request(RAF_SLICE);
}
static void tex_go(void) {
  tex_may_make = 1;
  tex_make_next();
}

void cd_on_store(uint32_t token, const uint8_t *p, int32_t len) {
  if (token == STORE_GAME) { game_stored(p, len); return; }
  int k = (int)token;
  if (k >= NTEX || tex_state[k] != TEX_ASKED) return;
  tex.answered[k] = cd_now();
  if (len > 0) { /* kept from an earlier load */
    tex.from_store[k] = 1;
    tex_state[k] = TEX_LANDED;
    tex_land(k, p, (uint32_t)len);
  } else {
    tex_state[k] = TEX_MISSING;
  }
  tex_make_next();
}

/* A texture made here, encoded: it lands, is kept for the next load, and the next is made. */
void cd_on_encoded(uint32_t token, const uint8_t *p, uint32_t len) {
  int k = (int)token;
  tex_state[k] = TEX_LANDED;
  tex_land(k, p, len);
  cd_store_put((uint32_t)k, tex.key[k], tex.klen[k], p, len); /* a cache: a failed put is not answered here */
  tex_making = 0;
  tex_make_next();
}

void cd_on_raf(uint32_t token, double t) {
  (void)t;
  if (token == RAF_PAINTED) { cd_raf_request(RAF_PAINTED2); return; }
  if (token == RAF_PAINTED2) { cd_timer_start(TEX_START_MS, TIMER_TEX); return; }
  double t0 = cd_now(), now = t0;
  int done = 0;
  while (!done && now - t0 < TEX_SLICE_MS) {
    if (tex.stage == TEX_FERN) done = fern_run(&tex.fern, 100000);
    else if (tex.stage == TEX_WOOD || tex.stage == TEX_WOOD_END) done = wood_run(&tex.wood, 2);
    else done = wool_run(&tex.wool, 100000);
    now = cd_now();
  }
  tex.ms[tex.stage] += now - t0;
  if (!done) { cd_raf_request(RAF_SLICE); return; }
  /* The pixels are copied during the call, but the next texture waits for the file: Chrome encodes
   * a PNG in idle time, and slices on every frame leave none (measured at CPU x4: the fern, made in
   * 50 ms, landed 700 ms later, behind the wool's slices). */
  if (tex.stage == TEX_FERN) cd_encode_image(tex.fern.px, (uint32_t)tex.fern_w, (uint32_t)tex.fern_h, 4, ZS("image/png"), TEX_FERN);
  if (tex.stage == TEX_WOOD || tex.stage == TEX_WOOD_END) cd_encode_image(tex.wood.px, WOOD_W, WOOD_H, 3, ZS("image/png"), (uint32_t)tex.stage);
  if (tex.stage == TEX_WOOL) cd_encode_image_in(tex.wool.px, WOOL_W, WOOL_H, 3, TEX_W, TEX_H, WOOL_X, WOOL_Y, ZS("image/jpeg"), TEX_WOOL);
}

static void textures_begin(void) {
  tex_seed = cd_seed ^ 0x9e3779b9u;
  cd_viewport_rec vp;
  cd_viewport(&vp);
  double dpr = vp.dpr100 > 0 ? vp.dpr100 / 100.0 : 1;
  tex.fern_w = (int32_t)(BACK_W * FERN_SCALE * dpr);
  tex.fern_h = (int32_t)(BACK_H * FERN_SCALE * dpr);
  if (tex.fern_w > FERN_APP_W) { tex.fern_w = FERN_APP_W; tex.fern_h = FERN_APP_H; }
  tex.nrect = wood_rects(tex.rect, &tex.nrect_now);
  tex_keys();
  tex_ask();
}

/* For the tests and the README, by texture k (fern, wood, wool): how long making it took on the
 * page's thread (0 when it came from the store), when it landed (cd_now: ms since navigation), and
 * whether it came from the store; k = NTEX: how many have landed. */
EXPORT("app_tex_ms") double app_tex_ms(int32_t k) { return k < NTEX ? tex.ms[k] : tex.landed; }
/* How many textures there are (fern, wood, wool, the end screen's wood): app_tex_ms(this) == this
 * when all have landed. */
EXPORT("app_tex_count") int32_t app_tex_count(void) { return NTEX; }
EXPORT("app_tex_at") double app_tex_at(int32_t k) { return tex.at[k]; }
/* When texture k was asked of the store (which = 0) and when the store answered (which = 1). */
EXPORT("app_tex_store") double app_tex_store(int32_t k, int32_t which) { return which ? tex.answered[k] : tex.asked[k]; }
EXPORT("app_tex_stored") int32_t app_tex_stored(int32_t k) { return tex.from_store[k]; }
/* The seed texture k was made from on this load (0 when it came from the store): the fern's
 * mulberry32 state; the wood's offsets are its first two draws, the wool's the next two
 * (tools/shots.mjs makes the TS textures from them). */
EXPORT("app_tex_seed") uint32_t app_tex_seed(int32_t k) { return tex.seed[k]; }
/* The wood's tile rectangles (x, y, w, h by field 0-3), for the test that every surface lies in one. */
EXPORT("app_wood_rects") int32_t app_wood_rects(void) { return tex.nrect; }
EXPORT("app_wood_rects_now") int32_t app_wood_rects_now(void) { return tex.nrect_now; } /* the table's; the rest are the end screen's */
EXPORT("app_wood_rect") int32_t app_wood_rect(int32_t k, int32_t f) {
  const TexRect *r = &tex.rect[k];
  return f == 0 ? r->x : f == 1 ? r->y : f == 2 ? r->w : r->h;
}

/* ---- the page -------------------------------------------------------------- */

void cd_main(void) {
  rng = cd_seed | 1;
  S_STYLE = cd_internz("style");
  for (int v = 1; v <= ACE_VALUE; v++) S_RANK[v] = cd_internz(CARD_RANK_LABEL[v]);
  for (int s = 0; s < NUM_SUITS; s++) S_SUIT[s] = cd_internz(CARD_SUIT_FACE[s].glyph);
  client_init(&CT, &SLOT);
  game_set_seed(rng);
  deal_fixture();

  build_face_template(T_FACE, 0);
  build_face_template(T_FACE10, 1);
  build_thin_template();

  cd_at(CD_MOUNT, 0);
  OPEN("div");
  cd_name_open(container = cd_id_new());
  ATTR("data-game-container", "true");
  CLASS("game-container");
  OPEN("div"); cd_name_open(bg_wool = cd_id_new()); CLASS("bg-wool"); STYLE("background-image: var(--tex-wool);"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--default"); CLOSE();
  OPEN("div"); CLASS("bg-soviet"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--soviet"); CLOSE();

  OPEN("div");
  cd_name_open(board = cd_id_new());
  CLASS("flex flex-1 flex-center");
  OPEN("p"); CLASS("title--game-display"); TEXT("Two of us"); CLOSE();

  OPEN("div");
  cd_name_open(deck_area.parent = cd_id_new());
  STYLE("display: flex; position: absolute; top: 0px; left: 0px; flex-direction: column; align-items: center; "
        "height: 240px; width: 100px; padding-top: 85px;");
  CLOSE();

  OPEN("div");
  STYLE("display: flex; position: absolute; top: 0px; right: 0px; flex-direction: column; justify-content: center; "
        "align-items: center; height: 240px; width: 100px;");
  OPEN("div"); cd_name_open(discard.parent = cd_id_new()); STYLE("position: relative;"); ATTR("data-location", "discard"); CLOSE();
  CLOSE();

  OPEN("div");
  cd_name_open(action_area = cd_id_new());
  ATTR("data-touch-interactive", "true");
  OPEN("div");
  cd_name_open(pills_box = cd_id_new());
  ATTR("data-touch-interactive", "true");
  pills.parent = pills_box;
  pills.owns_parent = 1;
  CLOSE();
  OPEN("div");
  ATTR("data-touch-interactive", "true"); ATTR("data-hand-container", "true"); ATTR("data-player-id", "u-me-0000");
  STYLE("width: 100%;");
  OPEN("div");
  cd_name_open(hand_plane = cd_id_new());
  ATTR("data-hand-plane", "true");
  hand.parent = hand_plane;
  hand.owns_parent = 1;
  CLOSE();
  CLOSE();
  CLOSE();

  cd_id table_area = cd_id_new();
  OPEN("div");
  cd_listen(table_area, CD_EV_CLICK);
  cd_tag_set(table_area, TAG_TABLE);
  CLASS("absolute flex flex-col items-center justify-center w-full");
  STYLE("top: 0px; bottom: 0px;");
  OPEN("div");
  cd_name_open(battles.parent = cd_id_new());
  ATTR("data-table-container", "true");
  STYLE("display: flex; flex-wrap: wrap; justify-content: center; gap: 10px; width: 100%; max-width: 300px; margin: 0px auto;");
  TEXT(" ");
  CLOSE();
  CLOSE();

  /* The seats go here, before the chat toggle. */
  OPEN("div");
  cd_name_open(seats_anchor = cd_id_new());
  CLASS("chat-toggle"); ATTR("data-chat-button", "true");
  OPEN("div"); CLASS("chat-toggle__icon"); OPEN("span"); CLASS(""); TEXT("\xF0\x9F\x93\x9E"); CLOSE(); CLOSE();
  CLOSE();
  CLOSE(); /* board */

  OPEN("button");
  CLASS("btn-icon btn-icon--left");
  cd_attr(S_STYLE, wood_style(SEED_BACK, 0));
  OPEN("span"); CLASS("btn-icon__symbol"); TEXT("<"); CLOSE();
  CLOSE();
  CLOSE(); /* container */


  game_asked = cd_now();
  cd_store_get(STORE_GAME, GAME_KEY, sizeof GAME_KEY - 1, CD_STORE_DEADLINE_MS);
  game_keep_canonical(&G);
  mark_shown();
  bot_check();
  render();
  if (!page_notex()) {
    textures_begin();                /* the store is asked now; what it has lands when read */
    cd_raf_request(RAF_PAINTED);     /* what it has not is made once the table has painted */
  }
}

