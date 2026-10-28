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
#include "strategy.h"
#include "../cardtext/card_text.h"
#include "textures.h"
#include <math.h>


#define EXPORT(n) __attribute__((export_name(n)))
/* The page's one setting: 1 when the URL asks for no textures (the golden comparisons). */
__attribute__((import_module("env"), import_name("notex"))) uint32_t page_notex(void);

extern uint32_t cd_seed;

/* ---- the game ------------------------------------------------------------ */

#define ME 0
#define BOT_STEP_MS 650   /* the bot's pace between its visible moves */

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
static int bot_pending;   /* a timer will drive the bot */
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

enum { TEX_FERN, TEX_WOOD, TEX_WOOL, TEX_DONE }; /* the textures, in the order they are made */

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
static cd_id battle_attack[52]; /* the attack face of each battle, by attack card id */

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
  if (covered) {
    face_open(b->defense, 0);
    cd_attr(S_STYLE, face_style(b->defense, FACE_DEFENSE, 0, 0, 0, 0));
    data_card(b->defense);
    CLOSE();
  }
  CLOSE();
  *data = (uint32_t)covered;
  return cell;
}

static void battle_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  const Battle *b = &V->battles[i];
  int covered = b->defense.suit >= 0;
  if ((uint32_t)covered == it->data) return;
  it->data = (uint32_t)covered; /* a battle is only ever covered, never uncovered: the attack tilts back */
  cd_id att = battle_attack[card_to_id(b->attack)];
  cd_set_style(att, ZS("transform"), ZS("translateX(-50%) rotate(-" COVER_TILT ")"));
  cd_set_style(att, ZS("z-index"), ZS("1"));
  cd_at(it->node, 0);
  face_open(b->defense, 0);
  cd_attr(S_STYLE, face_style(b->defense, FACE_DEFENSE, 0, 0, 0, 0));
  data_card(b->defense);
  CLOSE();
}

static void battle_destroy(void *ctx, cd_item *it) {
  (void)ctx;
  int aid = (int)it->key - 1;
  cd_id_free(battle_attack[aid]);
  battle_attack[aid] = 0;
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

/* TexturedSurface.tsx getTextureStyle: the wood once it is made (--tex-wood), the stripes before. */
static cd_str wood_style(double seed) {
  static SB s;
  int32_t x = (int32_t)__builtin_floor(seed * 1920), y = (int32_t)(seed * 1000 - 1080 * __builtin_floor(seed * 1000 / 1080));
  int32_t sx = (1920 - x % 1920) % 1920, sy = (1080 - y % 1080) % 1080;
  if (sx > 1920 - 260) x = 260;
  if (sy > 1080 - 60) y = 60;
  sb_reset(&s);
  sb_s(&s, "background-color: rgb(195, 63, 8); background-image: var(--tex-wood, repeating-linear-gradient(90deg, transparent, "
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
  cd_attr(S_STYLE, wood_style(PILL[p].seed));
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

static uint32_t current_pills(void) {
  if (V->status != GAME_STATUS_PLAYING || V->seats[ME].status != PLAYER_STATUS_IN) return 0;
  ClientPlay p;
  return ask_play(PLAY_TARGET_HAND, &p) == CLIENT_OK ? p.pills : 0;
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
static int mark_of(int seat) {
  const ViewSeat *p = &V->seats[seat];
  if (p->status == PLAYER_STATUS_OUT || V->status != GAME_STATUS_PLAYING) return MARK_NONE;
  if ((V->good_mask >> seat) & 1) return MARK_CHECK;
  if (V->defender == seat) return MARK_SHIELD;
  if (V->num_battles > 0 || seat == V->first_attacker)
    return seat == V->first_attacker ? MARK_LEAD : MARK_SWORD;
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
    int mark = mark_of(s);
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

static void result_row(const char *medal, int seat) {
  OPEN("div"); cd_class(seat == ME ? ZS("result-card result-card--current-user") : ZS("result-card "));
  OPEN("div"); CLASS("flex items-center gap-md flex-1 min-w-0");
  OPEN("div"); CLASS("result-card__rank"); OPEN("span"); CLASS(""); cd_text(cd_internz(medal)); CLOSE(); CLOSE();
  OPEN("div"); CLASS("flex items-baseline gap-sm min-w-0");
  OPEN("span"); cd_class(seat == ME ? ZS("result-card__name result-card__name--current") : ZS("result-card__name "));
  cd_text(cd_internz(SEAT_NAME[seat])); CLOSE();
  CLOSE(); CLOSE(); CLOSE();
}

static void render_end(void) {
  if (!cd_region_show(&end_screen, V->status == GAME_STATUS_GAME_OVER)) return;
  int fool = V->fool;
  cd_at(board, 0);
  OPEN("div");
  cd_name_open(end_screen.root);
  ATTR("data-end", "true");
  STYLE("position: absolute; inset: 0px; display: flex; flex-direction: column; align-items: center; "
        "justify-content: center; gap: 16px; z-index: 2000; background-color: rgba(0, 0, 0, 0.35);");
  OPEN("h1"); CLASS("win-screen__title");
  cd_text(fool == ME ? ZS("You are the fool") : fool < 0 ? ZS("A draw") : ZS("You won"));
  CLOSE();
  OPEN("div"); CLASS("win-screen__results"); STYLE("width: min(360px, 90vw);");
  OPEN("div"); CLASS("bg-wood"); cd_attr(S_STYLE, wood_style(0.42)); CLOSE();
  for (int s = 0; s < V->num_players; s++) if (s != fool) result_row("\xF0\x9F\xA5\x87", s);
  if (fool >= 0) result_row("\xF0\x9F\x83\x8F", fool);
  CLOSE();
  cd_id again = cd_id_new();
  OPEN("button"); cd_listen(again, CD_EV_CLICK); cd_tag_set(again, TAG_AGAIN);
  CLASS("btn-wood btn-wood--lg"); cd_attr(S_STYLE, wood_style(0.6));
  OPEN("span"); CLASS("btn-wood-text"); OPEN("span"); TEXT("Play again"); CLOSE(); CLOSE();
  CLOSE();
  CLOSE();
  cd_region_end(&end_screen); /* the button's id goes with the screen */
}

/* ---- one render ----------------------------------------------------------- */

static cd_id container;
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

enum { TIMER_BOT = 1, TIMER_TEX = 2 };
enum { RAF_SLICE = 0, RAF_PAINTED = 1, RAF_PAINTED2 = 2 };
/* Measured (tools/timing.mjs, BLANK=1, CPU x4): starting on a timer right after the first frame,
 * cdom.start resolved at 165 ms against 60 without textures; two frames and 50 ms later, at 60. */
#define TEX_START_MS 50
static void textures_begin(void);

static void schedule_bot(void) {
  if (bot_pending || G.status != GAME_STATUS_PLAYING || my_turn_owed()) return;
  bot_pending = 1;
  cd_timer_start(BOT_STEP_MS, TIMER_BOT);
}

/* One bot step: whatever the bot does up to its next visible move. */
static void bot_step(void) {
  bot_pending = 0;
  BotDriveOut out;
  bot_drive(&G, game_human_mask(&G), BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
  schedule_bot();
  render();
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
  nsel = 0;
  return 1;
}

/* The selection, played where the kernel says it goes: `target` is the table
 * (an attack or a transfer), a battle (a cover), or the Cover button. */
static int play_selection(int8_t target) {
  ClientPlay p;
  if (!nsel || ask_play(target, &p) != CLIENT_OK || p.move_type < 0) return 0;
  return play(p.move_type, p.cards, p.n_cards, p.move_type == MOVE_COVER ? p.attack_cards : 0);
}

static void on_pill(uint32_t bit) {
  Card cards[MAX_MOVE_CARDS];
  for (int i = 0; i < nsel; i++) cards[i] = card_of_id(sel[i]);
  if (bit == PLAY_PILL_ATTACK) play(AWIRE_ATTACK, cards, nsel, 0);
  else if (bit == PLAY_PILL_PASS) play(AWIRE_PASS, cards, nsel, 0);
  else if (bit == PLAY_PILL_COVER) play_selection(CLIENT_PLAY_COVER_BUTTON);
  else if (bit == PLAY_PILL_PICKUP) play(AWIRE_PICKUP, 0, 0, 0);
  else if (bit == PLAY_PILL_GOOD) play(AWIRE_GOOD, 0, 0, 0);
}

static void toggle(int cid) {
  for (int i = 0; i < nsel; i++)
    if (sel[i] == cid) { sel[i] = sel[--nsel]; return; }
  if (nsel < MAX_MOVE_CARDS) sel[nsel++] = (uint8_t)cid;
}

static void new_game(void) {
  nsel = 0;
  deal_new();
  render();
  schedule_bot();
  render();
}

void cd_on_event(const cd_event_rec *e) {
  if (e->type == CD_EV_RESIZE) { render(); return; } /* the hand is laid out for the new width */
  if (e->type == CD_EV_LOAD) { /* a texture landed; with the fern, the backs turn black under it, as CardBack's do */
    if (e->key == TEX_FERN) cd_set_style(CD_MOUNT, ZS("--tex-fern-bg"), ZS("#000000"));
    return;
  }
  if (e->type != CD_EV_CLICK) return;
  uint32_t tag = cd_tag((cd_id)e->id);
  if (tag & TAG_AGAIN) { new_game(); return; }
  if (V->status != GAME_STATUS_PLAYING) return;
  if (tag & TAG_CARD) toggle((int)(tag & 0xff));
  else if (tag & TAG_PILL) on_pill(tag & 0xff);
  else if (tag & TAG_BATTLE) {
    int aid = (int)(tag & 0xff), b = -1;
    for (int i = 0; i < V->num_battles; i++) if (card_to_id(V->battles[i].attack) == aid) b = i;
    if (b >= 0) play_selection((int8_t)b);
  } else if (tag & TAG_TABLE) play_selection(PLAY_TARGET_TABLE);
  render();
  schedule_bot();
  render();
}

void cd_on_timer(uint32_t token) {
  if (token == TIMER_BOT) bot_step();
  if (token == TIMER_TEX) textures_begin();
}

/* For the tests: the engine's reason for the last refused move (0 for none), and the string
 * table's size (the same strings are held in C's pool and in the shim). */
EXPORT("app_last_reject") int app_last_reject(void) { return last_reject; }
EXPORT("app_str_count") uint32_t app_str_count(void) { return cd_str_count(); }
EXPORT("app_str_bytes") uint32_t app_str_bytes(void) { return cd_str_bytes(); }
EXPORT("app_ids_live") uint32_t app_ids_live(void) { return cd_ids_live(); }
/* ---- the textures ----------------------------------------------------------
 * After the first paint (two frames, then a timer), in slices of about 8 ms on animation frames: the fern (the card backs),
 * the wood (the buttons), then the wool (the background). Each lands as a custom property on the
 * mount (cd_set_image), which the styles above read through var(). One 32 MiB block holds each
 * in turn: the wool's 3840 x 2160 pixels, or the wood's float colours and pixels together. */
enum { TEX_W = 3840, TEX_H = 2160, WOOD_W = 1920, WOOD_H = 1080, FERN_W = 1000, FERN_H = 1400 };
#define TEX_SLICE_MS 8.0
static struct { int stage; uint8_t *block; Fern fern; Wood wood; Wool wool; double ms[3]; } tex;
static uint32_t tex_rng, tex_seed;

static void tex_start(int stage) {
  tex.stage = stage;
  uint8_t *b = tex.block;
  if (stage == TEX_FERN) fern_begin(&tex.fern, b, b + FERN_W * FERN_H * 4, FERN_W, FERN_H, 5000000, tex_rng);
  if (stage == TEX_WOOD) {
    double ox = tex_random(&tex_rng) * 1000 - 500, oy = tex_random(&tex_rng) * 1000 - 500;
    float *col = (float *)b;
    uint8_t *px = b + WOOD_W * WOOD_H * 3 * 4;
    wood_begin(&tex.wood, col, (float *)(px + WOOD_W * WOOD_H * 4), px, WOOD_W, WOOD_H, ox, oy);
  }
  if (stage == TEX_WOOL) {
    double ox = tex_random(&tex_rng) * 1000 - 500, oy = tex_random(&tex_rng) * 1000 - 500;
    wool_begin(&tex.wool, b, TEX_W, TEX_H, ox, oy);
  }
}

void cd_on_raf(uint32_t token, double t) {
  (void)t;
  if (token == RAF_PAINTED) { cd_raf_request(RAF_PAINTED2); return; }
  if (token == RAF_PAINTED2) { cd_timer_start(TEX_START_MS, TIMER_TEX); return; }
  double t0 = cd_now(), now = t0;
  int done = 0;
  while (!done && now - t0 < TEX_SLICE_MS) {
    if (tex.stage == TEX_FERN) done = fern_run(&tex.fern, 100000);
    else if (tex.stage == TEX_WOOD) done = wood_run(&tex.wood, 2);
    else done = wool_run(&tex.wool, 100000);
    now = cd_now();
  }
  tex.ms[tex.stage] += now - t0;
  if (done) {
    if (tex.stage == TEX_FERN) cd_set_image(CD_MOUNT, ZS("--tex-fern"), tex.fern.px, FERN_W, FERN_H, ZS("image/png"), TEX_FERN);
    if (tex.stage == TEX_WOOD) cd_set_image(CD_MOUNT, ZS("--tex-wood"), tex.wood.px, WOOD_W, WOOD_H, ZS("image/png"), TEX_WOOD);
    if (tex.stage == TEX_WOOL) cd_set_image(CD_MOUNT, ZS("--tex-wool"), tex.wool.px, TEX_W, TEX_H, ZS("image/jpeg"), TEX_WOOL);
    if (tex.stage + 1 == TEX_DONE) { cd_free(tex.block); tex.block = 0; tex.stage = TEX_DONE; return; }
    tex_start(tex.stage + 1);
  }
  cd_raf_request(RAF_SLICE);
}

static void textures_begin(void) {
  tex_rng = tex_seed = cd_seed ^ 0x9e3779b9u;
  uint32_t wood = WOOD_W * WOOD_H * (3 * 4 + 4) + WOOD_H * 4, wool = TEX_W * TEX_H * 4;
  tex.block = cd_alloc(wood > wool ? wood : wool); /* 33.2 MB: one 32 MiB size class */
  tex_start(TEX_FERN);
  cd_raf_request(RAF_SLICE);
}

/* For the tests and the README: how long each texture took on the page's thread, in ms. */
EXPORT("app_tex_ms") double app_tex_ms(int32_t k) { return k < 3 ? tex.ms[k] : tex.stage; }
/* The seed the textures were made from: the fern's mulberry32 state, the wood's offsets its first
 * two draws, the wool's the next two (tools/shots.mjs makes the TS textures from it). */
EXPORT("app_tex_seed") uint32_t app_tex_seed(void) { return tex_seed; }

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
  OPEN("div"); CLASS("bg-wool"); STYLE("background-image: var(--tex-wool);"); CLOSE();
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
  cd_attr(S_STYLE, wood_style(0.2));
  OPEN("span"); CLASS("btn-icon__symbol"); TEXT("<"); CLOSE();
  CLOSE();
  CLOSE(); /* container */

  cd_listen_window(CD_EV_RESIZE);
  render();
  if (!page_notex()) cd_raf_request(RAF_PAINTED); /* the textures start once the table has painted */
}

