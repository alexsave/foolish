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
#include <math.h>


#define EXPORT(n) __attribute__((export_name(n)))
#define IMPORT(m, n) __attribute__((import_module(m), import_name(n)))

/* Host imports beyond cdom's DOM calls (shim/cdom.js in this directory). */
IMPORT("cdom", "timer") void js_timer(uint32_t ms, uint32_t token);
IMPORT("env", "vw") uint32_t js_vw(void);

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

static int my_turn_owed(void) { return bot_drive_eligible_mask(&G, game_human_mask(&G)) == 0; }

/* ---- strings ------------------------------------------------------------- */

static cd_str S_STYLE, S_DIV, S_SPAN, S_RANK[ACE_VALUE + 1], S_SUIT[NUM_SUITS];

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

/* Opens a face of c (the caller adds attributes and closes it). */
static void face_open(Card c, cd_str style, int thin) {
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
  cd_attr(S_STYLE, style);
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
  return sb_str(&s);
}

static void data_card(Card c) {
  static SB s;
  sb_reset(&s);
  sb_i(&s, c.suit); sb_c(&s, '-'); sb_i(&s, c.value);
  cd_attr(ZS("data-card"), cd_intern(s.b, s.n));
}

/* ---- card backs (CardBack.tsx, the plain red the golden shows) ----------- */

static cd_str back_style(uint32_t i, double rot, int stacked) {
  static SB s;
  sb_reset(&s);
  sb_s(&s, "position: absolute; ");
  sb_px(&s, "top: ", stacked ? -2.0 * i : 0);
  sb_px(&s, "left: ", stacked ? -1.0 * i : 0);
  sb_s(&s, "width: 50px; height: 70px; background-color: rgb(180, 14, 9); border: 1px solid rgb(139, 0, 0); "
           "border-radius: 5px; ");
  sb_s(&s, "z-index: "); sb_i(&s, (int32_t)i + 2);
  sb_s(&s, "; transform: rotate(90deg) rotate("); sb_f(&s, rot);
  sb_s(&s, "deg); transform-origin: center center; background-size: 100% 100%; background-repeat: no-repeat; overflow: hidden;");
  return sb_str(&s);
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
  cd_name(id);
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

/* Drops a list whose DOM went with a cd_clear: its ids are freed, nothing is emitted for its nodes. */
static void list_forget(cd_list *L) {
  for (uint32_t i = 0; i < L->n; i++) cd_id_free(L->items[i].node);
  L->n = 0;
}

#define BADGE "position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); margin: 0px; " \
  "color: var(--color-text-primary); font-size: 16px; font-weight: bold; text-shadow: 1px 1px 2px rgba(0,0,0,0.8); " \
  "pointer-events: none; z-index: 1003;"

/* ---- the stock and the trump (DeckAndFlipped.tsx) ------------------------ */

static cd_id deck_area, deck_badge_txt, deck_layers_box;
static cd_list deck_layers;
static int32_t deck_shape = -1, deck_badge = -1;

static void render_deck(void) {
  int32_t shape = R.show_deck_spot | R.show_deck_pile << 1 | R.show_flipped_slot << 2 |
                  R.show_flipped_card << 3 | R.show_trump_icon << 4 | (V->flipped.suit & 7) << 5 |
                  (V->flipped.value & 31) << 8 | (V->power_suit & 7) << 13;
  if (shape != deck_shape) { /* what the area holds changed: the deck ran out, a new deal */
    deck_shape = shape;
    cd_clear(deck_area);
    list_forget(&deck_layers);
    if (deck_badge_txt) { cd_id_free(deck_badge_txt); deck_badge_txt = 0; }
    if (deck_layers_box) { cd_id_free(deck_layers_box); deck_layers_box = 0; }
    deck_badge = -1;
    cd_at(deck_area, 0);
    if (R.show_deck_spot) {
      OPEN("div"); STYLE("position: relative;"); ATTR("data-location", "deck");
      OPEN("div"); STYLE("position: relative; width: 50px; height: 70px;");
      deck_layers.parent = deck_layers_box = cd_id_new();
      deck_layers.owns_parent = 1;
      cd_name(deck_layers_box);
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
      if (R.show_flipped_card) { face_open(V->flipped, face_style(V->flipped, FACE_STILL, 0, 0, 0, 0), 0); CLOSE(); }
      CLOSE();
    }
    if (R.show_trump_icon) {
      OPEN("div"); STYLE("display: flex; align-items: center; justify-content: center;");
      OPEN("span");
      static SB s;
      sb_reset(&s);
      sb_s(&s, "font-size: 64px; line-height: 1; color: ");
      sb_s(&s, CARD_SUIT_FACE[V->power_suit].red ? "#B32929;" : "#0A0A0A;");
      cd_attr(S_STYLE, sb_str(&s));
      cd_text(S_SUIT[V->power_suit]);
      CLOSE(); CLOSE();
    }
  }
  if (deck_layers_box) layers_update(&deck_layers, (uint32_t)R.deck_pile, &DECK_LAYERS);
  if (deck_badge_txt && deck_badge != R.deck_badge) cd_set_text_num(deck_badge_txt, (uint32_t)(deck_badge = R.deck_badge));
}

/* ---- the discard pile (DiscardPile.tsx) ---------------------------------- */

static cd_id discard_spot, discard_box, discard_txt;
static cd_list discard_layers;
static int32_t discard_n = -1;

static void render_discard(void) {
  int32_t n = V->discard_pile_length;
  if (n == discard_n) return;
  if (n == 0 || discard_n <= 0) { /* the pile appears or goes */
    cd_clear(discard_spot);
    list_forget(&discard_layers);
    if (discard_box) { cd_id_free(discard_box); cd_id_free(discard_txt); discard_box = discard_txt = 0; }
    if (n > 0) {
      cd_at(discard_spot, 0);
      OPEN("div"); STYLE("position: relative; width: 50px; height: 70px;");
      cd_name(discard_layers.parent = discard_box = cd_id_new());
      discard_layers.owns_parent = 1;
      CLOSE();
      cd_at(discard_spot, 0);
      OPEN("p"); STYLE(BADGE); cd_text_num((uint32_t)n); cd_name(discard_txt = cd_id_new()); CLOSE();
    }
  } else {
    cd_set_text_num(discard_txt, (uint32_t)n);
  }
  discard_n = n;
  if (discard_box) layers_update(&discard_layers, (uint32_t)n, &DISCARD_LAYERS);
}

static cd_str index_str(uint32_t i) {
  static SB s;
  sb_reset(&s);
  sb_i(&s, (int32_t)i);
  return cd_intern(s.b, s.n);
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
  face_open(b->attack, face_style(b->attack, FACE_ATTACK, 0, 0, 0, covered), 0);
  cd_name(battle_attack[aid] = cd_id_new());
  cd_attr(ZS("data-battle-index"), index_str(i));
  data_card(b->attack);
  CLOSE();
  if (covered) {
    face_open(b->defense, face_style(b->defense, FACE_DEFENSE, 0, 0, 0, 0), 0);
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
  it->data = (uint32_t)covered; /* a battle is only ever covered, never uncovered */
  cd_set_attr(battle_attack[card_to_id(b->attack)], S_STYLE, face_style(b->attack, FACE_ATTACK, 0, 0, 0, 1));
  cd_at(it->node, 0);
  face_open(b->defense, face_style(b->defense, FACE_DEFENSE, 0, 0, 0, 0), 0);
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
static double lift;
static cd_id action_area, pills_box, hand_plane;
static cd_list hand, pills;
static cd_str hand_style[52];
static int8_t hand_index[52];
static cd_str action_style, pills_style, plane_style;

static int selected(int cid) {
  for (int i = 0; i < nsel; i++) if (sel[i] == cid) return 1;
  return 0;
}

static int thin(const HandRect *r) { return r->w < 40 && r->h > 60; }

static cd_str hand_card_style(uint32_t i) {
  const HandRect *r = &HL.slot[i];
  Card c = V->my_hand[i];
  return face_style(c, FACE_HAND, r, HL.rows.height - r->y - r->h, selected(card_to_id(c)), 0);
}


static cd_id hand_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  Card c = V->my_hand[i];
  int cid = card_to_id(c);
  cd_id id = cd_id_new();
  face_open(c, hand_style[cid] = hand_card_style(i), thin(&HL.slot[i]));
  cd_listen(id, CD_EV_CLICK);
  cd_tag_set(id, TAG_CARD | (uint32_t)cid);
  cd_attr(ZS("data-card-index"), index_str(i));
  hand_index[cid] = (int8_t)i;
  ATTR("data-location", "hand");
  cd_attr(ZS("data-player-id"), ZS("u-me-0000"));
  data_card(c);
  ATTR("draggable", "true");
  CLOSE();
  *data = (uint32_t)cid;
  return id;
}

static void hand_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  int cid = (int)it->data;
  cd_str s = hand_card_style(i);
  if (s != hand_style[cid]) cd_set_attr(it->node, S_STYLE, hand_style[cid] = s);
  if (hand_index[cid] != (int8_t)i) cd_set_attr(it->node, ZS("data-card-index"), index_str((uint32_t)(hand_index[cid] = (int8_t)i)));
}

static void hand_destroy(void *ctx, cd_item *it) { (void)ctx; hand_style[it->data] = 0; }
static const cd_list_fns HAND_FNS = {0, hand_create, hand_patch, hand_destroy};

/* The pill column, top to bottom, as ActionButtons.tsx COLUMN. */
static const struct { uint32_t bit; const char *label; double seed; } PILL[] = {
  {PLAY_PILL_ATTACK, "Attack", 0.25}, {PLAY_PILL_COVER, "Cover", 0.45}, {PLAY_PILL_PASS, "Pass", 0.35},
  {PLAY_PILL_PICKUP, "Pickup", 0.15}, {PLAY_PILL_GOOD, "Good", 0.85},
};

/* TexturedSurface.tsx getTextureStyle without its wood picture (the golden's fallback). */
static cd_str wood_style(double seed) {
  static SB s;
  int32_t x = (int32_t)__builtin_floor(seed * 1920), y = (int32_t)(seed * 1000 - 1080 * __builtin_floor(seed * 1000 / 1080));
  int32_t sx = (1920 - x % 1920) % 1920, sy = (1080 - y % 1080) % 1080;
  if (sx > 1920 - 260) x = 260;
  if (sy > 1080 - 60) y = 60;
  sb_reset(&s);
  sb_s(&s, "background-color: rgb(195, 63, 8); background-image: repeating-linear-gradient(90deg, transparent, "
           "transparent 1px, rgba(0, 0, 0, 0.1) 1px, rgba(0, 0, 0, 0.1) 2px); background-size: 1920px 1080px; "
           "background-position: ");
  sb_i(&s, x); sb_s(&s, "px "); sb_i(&s, y);
  sb_s(&s, "px; background-repeat: repeat;");
  return sb_str(&s);
}

static uint32_t pill_order[5];
static cd_id pill_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  int p = (int)pill_order[i];
  cd_id wrap = cd_id_new(), btn = cd_id_new();
  OPEN("div");
  cd_name(wrap);
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
  double width = (double)js_vw() - 20; /* the plane: the window less cards.css's 10px gutters */
  hand_layout(&WEB_HAND, V->my_hand_count, width, &HL);
  HandLayout empty;
  hand_layout(&WEB_HAND, 0, width, &empty);
  lift = HL.rows.height - empty.rows.height;

  static SB s;
  sb_reset(&s);
  sb_s(&s, "display: flex; flex-direction: column; position: absolute; bottom: max(10px, env(safe-area-inset-bottom)); "
           "left: 0px; right: 0px; justify-content: end; align-items: center; ");
  sb_px(&s, "height: ", 200 + lift);
  cd_str st = sb_str(&s);
  if (st != action_style) cd_set_attr(action_area, S_STYLE, action_style = st);
  sb_reset(&s);
  sb_s(&s, "position: absolute; ");
  sb_px(&s, "bottom: ", 90 + lift);
  sb_s(&s, "right: 20px; display: flex; flex-direction: column; gap: 5px; z-index: 999;");
  st = sb_str(&s);
  if (st != pills_style) cd_set_attr(pills_box, S_STYLE, pills_style = st);
  sb_reset(&s);
  sb_s(&s, "position: relative; width: 100%; ");
  sb_px(&s, "height: ", HL.rows.height);
  st = sb_str(&s);
  if (st != plane_style) cd_set_attr(hand_plane, S_STYLE, plane_style = st);

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
  cd_id root, role, hand, count_box, count_txt;
  cd_list minis;
  int mark, count;
  cd_str root_style;
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
  const char *size = mark == MARK_SHIELD ? "33" : mark == MARK_CHECK ? "26" : "40";
  cd_attr(ZS("width"), cd_internz(size)); cd_attr(ZS("height"), cd_internz(size));
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
static cd_str mini_style(uint32_t i, int n) {
  static SB s;
  sb_reset(&s);
  sb_s(&s, "box-sizing: border-box; background-color: rgb(180, 14, 9); width: 25px; height: 35px; border-radius: 3px; "
           "border: 1px solid rgb(139, 0, 0); position: absolute; top: 0px; ");
  sb_px(&s, "left: ", 50 + (i - mid_of(n)) * 10 - 12.5);
  sb_s(&s, "z-index: "); sb_i(&s, (int32_t)i);
  sb_s(&s, "; box-shadow: 0 1px 2px rgba(0,0,0,0.3); background-size: 100% 100%; background-repeat: no-repeat; overflow: hidden;");
  return sb_str(&s);
}

static cd_id mini_create(void *ctx, uint32_t i, uint32_t *data) {
  SeatUI *u = ctx;
  cd_id id = cd_id_new();
  OPEN("div"); cd_name(id); cd_attr(S_STYLE, (cd_str)(*data = mini_style(i, u->count))); CLOSE();
  return id;
}
static void mini_patch(void *ctx, uint32_t i, cd_item *it) {
  SeatUI *u = ctx;
  cd_str s = mini_style(i, u->count);
  if (s != it->data) cd_set_attr(it->node, S_STYLE, (cd_str)(it->data = s));
}
static void mini_destroy(void *ctx, cd_item *it) { (void)ctx; (void)it; }

static cd_str seat_style(int seat) {
  static SB s;
  int v = (seat - V->my_seat + n_seats) % n_seats;
  double rad = 2 * M_PI * v / n_seats;
  sb_reset(&s);
  sb_s(&s, "position: absolute; top: "); sb_f(&s, cos(rad) * 35 + 50);
  sb_s(&s, "%; left: "); sb_f(&s, -sin(rad) * 35 + 50);
  sb_s(&s, "%; display: flex; flex-direction: column; align-items: center; width: 80px; height: 91px; ");
  if (v == 0 && lift > 0) { sb_s(&s, "transform: translate(-50%, calc(-50% - "); sb_f(&s, lift); sb_s(&s, "px));"); }
  else sb_s(&s, "transform: translate(-50%, -50%);");
  return sb_str(&s);
}

#define ROLE_ROW "height: 40px; width: 40px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; " \
  "transform: scaleX(1); will-change: transform;"
#define NAME_ROW "margin: 0px; font-size: 12px; color: var(--color-text-primary); text-shadow: var(--text-shadow-label); " \
  "white-space: nowrap; text-align: center; height: 16px; line-height: 16px; flex-shrink: 0; display: flex; " \
  "align-items: center; justify-content: center; position: relative;"
#define COUNT_BOX "position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: var(--color-text-primary); " \
  "font-size: 10px; font-weight: bold; z-index: 100; pointer-events: none; text-shadow: 1px 1px 2px rgba(0,0,0,0.8);"

static void seats_build(void) {
  for (int s = 0; s < n_seats; s++) { /* a new deal: the old ring goes */
    SeatUI *u = &seat_ui[s];
    list_forget(&u->minis);
    cd_id_free(u->role); cd_id_free(u->hand);
    if (u->count_box) { cd_id_free(u->count_box); cd_id_free(u->count_txt); }
    cd_remove(u->root);
  }
  n_seats = V->num_players;
  for (int s = 0; s < n_seats; s++) {
    SeatUI *u = &seat_ui[s];
    __builtin_memset(u, 0, sizeof *u);
    u->mark = -1;
    cd_at(board, seats_anchor);
    OPEN("div");
    cd_name(u->root = cd_id_new());
    cd_attr(S_STYLE, u->root_style = seat_style(s));
    OPEN("div");
    cd_name(u->role = cd_id_new());
    cd_attr(ZS("data-role-seat"), index_str((uint32_t)s));
    ATTR("data-role-mark", "");
    STYLE(ROLE_ROW);
    CLOSE();
    OPEN("div"); STYLE(NAME_ROW); cd_text(cd_internz(SEAT_NAME[s])); CLOSE();
    OPEN("div");
    cd_name(u->hand = cd_id_new());
    STYLE("display: flex; flex-direction: row; align-items: center; justify-content: center; position: relative; "
          "height: 35px; width: 100px; flex-shrink: 0;");
    if (s != V->my_seat) { ATTR("data-location", "hand"); cd_attr(ZS("data-player-id"), cd_internz(SEAT_ID[s])); }
    CLOSE();
    CLOSE();
    u->minis.parent = u->hand;
  }
}

static void render_seats(void) {
  if (n_seats != V->num_players) seats_build();
  for (int s = 0; s < n_seats; s++) {
    SeatUI *u = &seat_ui[s];
    cd_str st = seat_style(s);
    if (st != u->root_style) cd_set_attr(u->root, S_STYLE, u->root_style = st);
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
      const cd_list_fns f = {u, mini_create, mini_patch, mini_destroy};
      cd_list_update(&u->minis, keys, (uint32_t)n, &f);
      if (n > 0 && !u->count_box) {
        cd_at(u->hand, 0);
        OPEN("div"); cd_name(u->count_box = cd_id_new()); STYLE(COUNT_BOX);
        cd_text_num((uint32_t)n); cd_name(u->count_txt = cd_id_new());
        CLOSE();
      } else if (n > 0) {
        cd_set_text_num(u->count_txt, (uint32_t)n);
        cd_move(u->count_box, 0); /* the count stays the mini hand's last child, after any card the list appended */
      } else if (u->count_box) {
        cd_remove(u->count_box); cd_id_free(u->count_txt);
        u->count_box = u->count_txt = 0;
      }
    }
  }
}

/* ---- the end of the game -------------------------------------------------- */

static cd_id end_box;

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
  int over = V->status == GAME_STATUS_GAME_OVER;
  if (over == (end_box != 0)) return;
  if (!over) { cd_remove(end_box); end_box = 0; return; }
  int fool = V->fool;
  cd_at(board, 0);
  OPEN("div");
  cd_name(end_box = cd_id_new());
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

enum { TIMER_BOT = 1 };

static void schedule_bot(void) {
  if (bot_pending || G.status != GAME_STATUS_PLAYING || my_turn_owed()) return;
  bot_pending = 1;
  js_timer(BOT_STEP_MS, TIMER_BOT);
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
  if (end_box) { cd_remove(end_box); end_box = 0; }
  nsel = 0;
  deal_new();
  render();
  schedule_bot();
  render();
}

void cd_on_event(const cd_event_rec *e) {
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

EXPORT("cd_timer") void cd_timer(uint32_t token) {
  if (token == TIMER_BOT) bot_step();
  cd_flush();
}

EXPORT("app_resize") void app_resize(void) {
  render();
  cd_flush();
}

/* For the test: the engine's reason for the last refused move, 0 for none. */
EXPORT("app_last_reject") int app_last_reject(void) { return last_reject; }

/* ---- the page -------------------------------------------------------------- */

void cd_main(void) {
  rng = cd_seed | 1;
  S_STYLE = cd_internz("style");
  S_DIV = cd_internz("div");
  S_SPAN = cd_internz("span");
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
  cd_name(container = cd_id_new());
  ATTR("data-game-container", "true");
  CLASS("game-container");
  OPEN("div"); CLASS("bg-wool"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--default"); CLOSE();
  OPEN("div"); CLASS("bg-soviet"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--soviet"); CLOSE();

  OPEN("div");
  cd_name(board = cd_id_new());
  CLASS("flex flex-1 flex-center");
  OPEN("p"); CLASS("title--game-display"); TEXT("Two of us"); CLOSE();

  OPEN("div");
  cd_name(deck_area = cd_id_new());
  STYLE("display: flex; position: absolute; top: 0px; left: 0px; flex-direction: column; align-items: center; "
        "height: 240px; width: 100px; padding-top: 85px;");
  CLOSE();

  OPEN("div");
  STYLE("display: flex; position: absolute; top: 0px; right: 0px; flex-direction: column; justify-content: center; "
        "align-items: center; height: 240px; width: 100px;");
  OPEN("div"); cd_name(discard_spot = cd_id_new()); STYLE("position: relative;"); ATTR("data-location", "discard"); CLOSE();
  CLOSE();

  OPEN("div");
  cd_name(action_area = cd_id_new());
  ATTR("data-touch-interactive", "true");
  OPEN("div");
  cd_name(pills_box = cd_id_new());
  ATTR("data-touch-interactive", "true");
  pills.parent = pills_box;
  pills.owns_parent = 1;
  CLOSE();
  OPEN("div");
  ATTR("data-touch-interactive", "true"); ATTR("data-hand-container", "true"); ATTR("data-player-id", "u-me-0000");
  STYLE("width: 100%;");
  OPEN("div");
  cd_name(hand_plane = cd_id_new());
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
  cd_name(battles.parent = cd_id_new());
  ATTR("data-table-container", "true");
  STYLE("display: flex; flex-wrap: wrap; justify-content: center; gap: 10px; width: 100%; max-width: 300px; margin: 0px auto;");
  TEXT(" ");
  CLOSE();
  CLOSE();

  /* The seats go here, before the chat toggle. */
  OPEN("div");
  cd_name(seats_anchor = cd_id_new());
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

  render();
}

