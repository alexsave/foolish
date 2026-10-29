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
#include "anim_run.h"
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
/* The board whose battles the row draws: V, or while a card is coming down onto the table the board
 * of the step that brings it, so the row makes room as it flies (React's arrivingPiles). */
static const TableView *RV;

/* What the run in the air shows besides its landed board (README, Animations), worked out from the
 * kernel's frame before every render (run_show): the stock's cards in the air, the cards hidden
 * where their flight left and where it lands, the attacks turning under a cover in flight, the room
 * my hand makes for cards flying into it, and the flight's duration. All zero with no run. */
static struct {
  int from_deck, to_flipped, trump_out;
  uint64_t veil_hand, veil_table; /* by card id (card_to_id): hidden in my hand, on the table */
  uint64_t covering;              /* attack ids a cover in the air lands on */
  int arriving;                   /* cards flying into my hand */
  int flight_ms;                  /* the beat in the air's duration */
} SHOW;
static int veiled(uint64_t set, int cid) { return (int)((set >> cid) & 1); }
static cd_id container, bg_wool; /* the table's root, and the wool behind it */

/* The centre of a node as drawn now (cdom's cd_rect: the browser lays out first if it must). */
static void centre_of(cd_id id, double *x, double *y) {
  double r[4];
  cd_rect(id, r);
  *x = r[0] + r[2] / 2;
  *y = r[1] + r[3] / 2;
}

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
 * bout_2p_attacker.html: the same fields its fixture builder writes. The test build composes
 * others on the same stock (a table, my hand, the bot's). */
static void compose(const char *table, const char *mine, const char *bots);
#ifdef TABLE_DEBUG
/* The test build's boards (app_debug_board, and ?board=k on the page as the first board): 0 the
 * golden's with 6d covered by Kd, 1 and 2 boards where I defend (README, the gesture checks), 3 all
 * covered, 4 the golden's board with the seats swapped: I defend it, as Anna does in the golden
 * bout_2p_defender.html. */
static void debug_compose(int32_t k);
__attribute__((import_module("env"), import_name("board"))) uint32_t page_board(void);
#endif
static void deal_fixture(void) {
#ifdef TABLE_DEBUG
  if (page_board()) { debug_compose((int32_t)page_board()); return; }
#endif
  compose("6h/8h 6d", "6s 7h Qd Ad", "9h Th Jh 6c");
}
static void compose(const char *table, const char *mine, const char *bots) {
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
  n = parse(table, c, cov, MAX_BATTLES);
  G.num_battles = (int8_t)n;
  for (int i = 0; i < n; i++) G.table_battles[i] = (Battle){c[i], cov[i]};
  const char *hands[] = {mine, bots};
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
#define COVER_RAD 0.19634954084936207 /* TableBattles.tsx COVER_ROTATION_RAD, pi / 16 */
#define COVER_TILT "0.19634954084936207rad"
#define EASE "cubic-bezier(0.25, 0.46, 0.45, 0.94)" /* FlightCard.tsx EASE: the flights' curve, and the row's */
#define FLIGHT_ARM_MS 25 /* FlightCard.tsx: a flight stands one paint at its start before it moves */

/* A battle's attack turns with the cover flying onto it: over the flight's duration, on its curve,
 * starting when the flight is armed (TableBattles.tsx attackCardStyle); 0 ms with nothing flying. */
static void sb_attack_transition(SB *s, int ms) {
  sb_s(s, "transform "); sb_i(s, ms); sb_s(s, "ms " EASE " "); sb_i(s, ms > 0 ? FLIGHT_ARM_MS : 0);
  sb_s(s, "ms, box-shadow 0.2s ease-in-out");
}

/* `tilted`, an attack's: bit 0 turned out of its cover's way, bit 1 covered (it lies under). */
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
  if (kind == FACE_ATTACK) { sb_s(&s, "transition: "); sb_attack_transition(&s, SHOW.flight_ms); sb_s(&s, "; "); }
  else sb_s(&s, kind == FACE_HAND ? "transition: all 0.1s ease, visibility 0s; "
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
    sb_s(&s, tilted & 1 ? "bottom: 5px; left: 50%; transform: translateX(-50%) rotate(-" COVER_TILT "); transform-origin: center bottom; "
                        : "bottom: 5px; left: 50%; transform: translateX(-50%); transform-origin: center bottom; ");
    sb_s(&s, tilted & 2 ? "z-index: 1;" : "z-index: 2;");
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

static cd_id deck_badge_txt, deck_layers_box, deck_spot, flipped_slot; /* the last two: where a flight leaves or lands */
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
    deck_badge_txt = deck_layers_box = deck_spot = flipped_slot = 0;
    deck_badge = -1;
    cd_region_show(&deck_area, 1);
    cd_at(deck_area.parent, 0);
    if (R.show_deck_spot) {
      OPEN("div"); STYLE("position: relative;"); ATTR("data-location", "deck");
      cd_name_open(deck_spot = cd_id_new());
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
      cd_name_open(flipped_slot = cd_id_new());
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
static cd_id battle_cell[52];    /* its cell, the box the row lays out and glides */
/* What each battle shows now, by attack id: the attack turned (covered, or a cover flying onto it),
 * the attack and its cover hidden (their flight is up), and the transition the turn runs on. */
static struct { int8_t tilt, veil_a, veil_d, amber; int32_t ms; } bshown[52];
/* The drop a drag would make now, while it is a game action: the kernel's move, else move_type -1
 * (render_drag keeps it; the battles and the drop cells read it). */
static const ClientPlay *drop_now;

/* A battle item's data: its cover's card id + 1, 0 while uncovered. */
static uint32_t cover_data(const Battle *b) { return b->defense.suit >= 0 ? (uint32_t)card_to_id(b->defense) + 1 : 0; }

/* A card a flight carries is hidden where it left and where it lands, at once and shown at once
 * (CardFace.tsx sets visibility hidden over its own style; its "no pointer events" and "no
 * transition" change nothing here: a hidden card is never hit, and nothing moves it while hidden). */
static void veil_set(cd_id id, int on) { cd_set_style(id, ZS("visibility"), on ? ZS("hidden") : ZS("")); }

static void defense_add(int aid, Card d) {
  face_open(d, 0);
  cd_attr(S_STYLE, face_style(d, FACE_DEFENSE, 0, 0, 0, 0));
  cd_name_open(battle_defense[aid] = cd_id_new());
  data_card(d);
  CLOSE();
  bshown[aid].veil_d = 0;
}

/* The battle's turn, veils and transition as the run has them now: only what changed. */
static void battle_show(int aid, const Battle *b) {
  cd_id att = battle_attack[aid];
  int covered = b->defense.suit >= 0;
  if (SHOW.flight_ms != bshown[aid].ms) { /* first, so a turn set with it runs on it */
    static SB t;
    sb_reset(&t);
    sb_attack_transition(&t, bshown[aid].ms = SHOW.flight_ms);
    cd_set_style(att, ZS("transition"), sb_tmp(&t));
  }
  int tilt = covered || veiled(SHOW.covering, aid);
  if (tilt != bshown[aid].tilt)
    cd_set_style(att, ZS("transform"), (bshown[aid].tilt = (int8_t)tilt) ? ZS("translateX(-50%) rotate(-" COVER_TILT ")") : ZS("translateX(-50%)"));
  int va = veiled(SHOW.veil_table, aid);
  if (va != bshown[aid].veil_a) veil_set(att, bshown[aid].veil_a = (int8_t)va);
  int vd = covered && veiled(SHOW.veil_table, card_to_id(b->defense));
  if (covered && vd != bshown[aid].veil_d) veil_set(battle_defense[aid], bshown[aid].veil_d = (int8_t)vd);
  /* a drag's cover would land here (TableBattles.tsx isCardBeingCovered): the cell in amber */
  int amber = 0;
  if (drop_now && drop_now->move_type == MOVE_COVER)
    for (int k = 0; k < drop_now->n_cards; k++) amber |= card_to_id(drop_now->attack_cards[k]) == aid;
  if (amber != bshown[aid].amber) {
    bshown[aid].amber = (int8_t)amber;
    cd_set_style(battle_cell[aid], ZS("border"), amber ? ZS("3px solid rgb(210, 144, 2)") : ZS(""));
    cd_set_style(battle_cell[aid], ZS("background-color"), amber ? ZS("rgba(210, 144, 2, 0.1)") : ZS(""));
  }
}

static cd_id battle_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  const Battle *b = &RV->battles[i];
  int covered = b->defense.suit >= 0;
  int aid = card_to_id(b->attack);
  int tilt = covered || veiled(SHOW.covering, aid);
  cd_id cell = cd_id_new();
  battle_cell[aid] = cell;
  OPEN("div");
  cd_name_open(cell);
  STYLE("display: flex; flex-direction: column; align-items: center; position: relative; width: 60px; "
        "height: 80px; margin: 5px; justify-content: center;");
  ATTR("data-location", "table");
  face_open(b->attack, 0);
  cd_attr(S_STYLE, face_style(b->attack, FACE_ATTACK, 0, 0, 0, tilt | covered << 1));
  /* the attack card is the battle's target, for a click and under a drag (data-battle-index) */
  cd_listen(battle_attack[aid] = cd_id_new(), CD_EV_CLICK);
  cd_tag_set(battle_attack[aid], TAG_BATTLE | (uint32_t)aid);
  cd_attr(ZS("data-battle-index"), index_str(i));
  data_card(b->attack);
  CLOSE();
  if (covered) defense_add(aid, b->defense);
  CLOSE();
  bshown[aid].tilt = (int8_t)tilt;
  bshown[aid].ms = SHOW.flight_ms;
  bshown[aid].veil_a = bshown[aid].veil_d = bshown[aid].amber = 0;
  battle_show(aid, b);
  *data = cover_data(b);
  return cell;
}

/* A battle that survived: its cover when it changed (covered; or, when the board is replaced by a
 * load, uncovered or covered by another card), then its turn and veils. */
static void battle_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  const Battle *b = &RV->battles[i];
  uint32_t want = cover_data(b);
  int aid = card_to_id(b->attack);
  if (want != it->data) {
    if (it->data) { cd_remove(battle_defense[aid]); battle_defense[aid] = 0; }
    it->data = want;
    cd_set_style(battle_attack[aid], ZS("z-index"), want ? ZS("1") : ZS("2"));
    if (want) { cd_at(it->node, 0); defense_add(aid, b->defense); }
  }
  battle_show(aid, b);
}

static void battle_destroy(void *ctx, cd_item *it) {
  (void)ctx;
  int aid = (int)it->key - 1;
  cd_id_free(battle_attack[aid]);
  battle_attack[aid] = battle_cell[aid] = 0;
  if (battle_defense[aid]) { cd_id_free(battle_defense[aid]); battle_defense[aid] = 0; }
}

static const cd_list_fns BATTLE_FNS = {0, battle_create, battle_patch, battle_destroy};

/* ---- the row's glide (TableBattles.tsx, the FLIP) ---------------------------------------------
 * The cells are centred, so a pile arriving or leaving moves every other one, and CSS cannot
 * transition a flex reflow. So each cell is pinned where it is DRAWN (its box before the change,
 * a glide still running included), then let go to where the new layout put it, over the duration
 * of the step that opened last and on the flights' curve: the row moves with the card. Measured
 * only when the row's cells change. */
static uint32_t row_keys[MAX_BATTLES], n_row_keys;
static double cell_rest[52][2];  /* each cell's box at rest (left, top), as last laid out */
static int8_t cell_glided[52];   /* a glide was started on it: its transition is cut before the next measure */
static double cell_glide_end[52]; /* when that glide is over (cd_now ms): until then the cell is not at its rest */
static int row_ms;               /* React's rowMs: the duration of the step that opened last */

/* The drop cells a drag holds open beside the row for an attack or a transfer, one a card
 * (TableBattles.tsx emptyDropZones): dashed amber with a "+", after the battles; each is a target
 * for the drop past the last battle, which the kernel reads as the table. */
static cd_id drop_cell[MAX_MOVE_CARDS];
static int n_drop_cells;
static void drop_cells_set(int want, uint32_t n_battles) {
  while (n_drop_cells > want) cd_remove(drop_cell[--n_drop_cells]);
  battles.end = n_drop_cells ? drop_cell[0] : 0; /* the battles stay before them */
  while (n_drop_cells < want) {
    cd_id id = cd_id_new();
    cd_at(battles.parent, 0);
    OPEN("div");
    cd_name_open(id);
    ATTR("data-location", "table");
    cd_attr(ZS("data-battle-index"), index_str(n_battles + (uint32_t)n_drop_cells));
    STYLE("display: flex; flex-direction: column; align-items: center; position: relative; width: 60px; height: 80px; "
          "margin: 5px; justify-content: center; border: 3px dashed rgb(210, 144, 2); background-color: rgba(210, 144, 2, 0.1); "
          "border-radius: 8px;");
    OPEN("div");
    STYLE("width: 40px; height: 60px; border: 2px dashed rgb(210, 144, 2); border-radius: 4px; display: flex; "
          "align-items: center; justify-content: center; font-size: 24px; color: rgb(210, 144, 2); opacity: 0.7;");
    TEXT("+");
    CLOSE();
    CLOSE();
    drop_cell[n_drop_cells++] = id;
    battles.end = drop_cell[0];
  }
}

static void render_table(void) {
  uint32_t n = (uint32_t)RV->num_battles;
  for (uint32_t i = 0; i < n; i++) keys[i] = (uint32_t)card_to_id(RV->battles[i].attack) + 1;
  int same = n == n_row_keys && !__builtin_memcmp(keys, row_keys, n * sizeof keys[0]);
  int cells = drop_now && (drop_now->move_type == MOVE_ATTACK || drop_now->move_type == MOVE_PASS) ? drop_now->n_cards : 0;
  int cells_change = cells != n_drop_cells;
  double drawn[52][2];
  int8_t had[52] = {0};
  double r[4];
  if (!same) /* where each cell is drawn now, the drop cells a drag held open included (React's rest) */
    for (uint32_t i = 0; i < n_row_keys; i++) {
      int aid = (int)row_keys[i] - 1;
      if (!battle_cell[aid]) continue;
      cd_rect(battle_cell[aid], r);
      drawn[aid][0] = r[0]; drawn[aid][1] = r[1];
      had[aid] = 1;
    }
  if (cells_change && cells < n_drop_cells) drop_cells_set(cells, n); /* gone before the list: its tail goes after the battles */
  cd_list_update(&battles, keys, n, &BATTLE_FNS);
  if (cells_change && cells > n_drop_cells) drop_cells_set(cells, n);
  else if (n_drop_cells && !same) /* their indices follow the battle count */
    for (int k = 0; k < n_drop_cells; k++) cd_set_attr(drop_cell[k], ZS("data-battle-index"), index_str(n + (uint32_t)k));
  if (same && cells_change) { /* the drop cells make room at once, with no glide (React's ms 0): every glide cut, the rests measured */
    for (uint32_t i = 0; i < n; i++) {
      int aid = (int)keys[i] - 1;
      if (cell_glided[aid]) { cd_set_style(battle_cell[aid], ZS("transition"), ZS("none")); cd_set_style(battle_cell[aid], ZS("transform"), ZS("")); cell_glided[aid] = 0; }
    }
    for (uint32_t i = 0; i < n; i++) {
      int aid = (int)keys[i] - 1;
      cd_rect(battle_cell[aid], r);
      cell_rest[aid][0] = r[0]; cell_rest[aid][1] = r[1];
    }
    return;
  }
  if (same) return;
  __builtin_memcpy(row_keys, keys, n * sizeof keys[0]);
  n_row_keys = n;
  int moved[MAX_BATTLES], n_moved = 0;
  for (uint32_t i = 0; i < n; i++) { /* where the new layout puts each cell, every glide cut */
    int aid = (int)keys[i] - 1;
    if (cell_glided[aid]) { cd_set_style(battle_cell[aid], ZS("transition"), ZS("none")); cell_glided[aid] = 0; }
  }
  for (uint32_t i = 0; i < n; i++) {
    int aid = (int)keys[i] - 1;
    cd_rect(battle_cell[aid], r);
    cell_rest[aid][0] = r[0]; cell_rest[aid][1] = r[1];
    if (!had[aid]) continue; /* a pile that was not down has nowhere to come from */
    double dx = drawn[aid][0] - r[0], dy = drawn[aid][1] - r[1];
    if (__builtin_fabs(dx) < 0.5 && __builtin_fabs(dy) < 0.5) continue;
    static SB t;
    sb_reset(&t);
    sb_s(&t, "translate("); sb_num(&t, dx, CD_UNIT_PX); sb_s(&t, ", "); sb_num(&t, dy, CD_UNIT_PX); sb_s(&t, ")");
    cd_set_style(battle_cell[aid], ZS("transition"), ZS("none"));
    cd_set_style(battle_cell[aid], ZS("transform"), sb_tmp(&t));
    moved[n_moved++] = aid;
  }
  if (!n_moved) return;
  cd_rect(battle_cell[moved[0]], r); /* the pinned place is the style the glide starts from */
  for (int k = 0; k < n_moved; k++) {
    static SB t;
    sb_reset(&t);
    sb_s(&t, "transform "); sb_i(&t, row_ms); sb_s(&t, "ms " EASE);
    cd_set_style(battle_cell[moved[k]], ZS("transition"), sb_tmp(&t));
    cd_set_style(battle_cell[moved[k]], ZS("transform"), ZS(""));
    cell_glided[moved[k]] = 1;
    cell_glide_end[moved[k]] = cd_now() + row_ms;
  }
}

/* ---- my hand (ActionButtons.tsx CardDiv, handShape.tsx) ------------------- */

static const HandMetrics WEB_HAND = {50, 20, 70, 2, 6, 0, 34};
static HandLayout HL;
static double lift, act_h = -1, pills_b = -1, plane_h = -1; /* lift, and the three lengths as drawn */
static cd_id action_area, pills_box, hand_plane;
static cd_list hand, pills;
/* What each hand card shows now, by card id: its box, its bottom, its border red (selected, or
 * dragged off the hand), dimmed (dragged within it), its index, hidden (a flight carries it), and
 * its node (where a flight leaves from). */
static struct { double x, w, h, bottom; int8_t sel, dim, index, veil; cd_id node; } shown[52];

/* My hand in the order it is drawn (React's localHandOrder): the board's cards in the order the
 * table remembers (client_hand_remember, client_hand_shown: a card keeps its slot while it is away,
 * a new one goes to the end), which drags and the keyboard rearrange (client_hand_swap). */
static uint8_t hand_memory[52];
static int hand_memory_n;
static Card HAND[MAX_HAND_SIZE];
static int hand_n;
static void hand_order(void) {
  hand_memory_n = client_hand_remember(hand_memory, hand_memory_n, (int)sizeof hand_memory, V->my_hand, V->my_hand_count);
  hand_n = V->my_hand_count;
  client_hand_shown(hand_memory, hand_memory_n, V->my_hand, hand_n, HAND);
}

/* A drag of a hand card (DragContext.tsx): pressed on a card, and from 10 px a drag, which within
 * the hand's area rearranges it and outside it is a game action whose drop the kernel reads. */
static struct {
  int on;              /* a card is pressed (isDraggingCard) */
  int index;           /* its index in the hand as drawn (draggedCardIndex, following it as it swaps) */
  int x0, y0;          /* where it was pressed */
  int32_t t0;          /* when: the press's own time (cd_event_rec.ms), as React reads e.timeStamp */
  int moving;          /* it has moved more than 10 px (isActuallyDragging) */
  int swapped;         /* the hand was rearranged by it (hasSwapped) */
  int action;          /* outside the hand's area: a game action (isDraggingForGameAction) */
  int cid;             /* the card dragged for the action (draggedCard) */
  int x, y;            /* the pointer (currentCursorPos) */
  double area_top;     /* the hand's area starts here: the plane's top less a card's height */
  int8_t target;       /* client_drop_target where the pointer is */
  int asked;           /* PLAY is the kernel's answer for the target and cards below */
  int8_t asked_target, asked_n;
  uint8_t asked_cards[MAX_MOVE_CARDS];
  ClientPlay play;     /* what the drop would be (determineGameAction) */
} DRAG;
/* The keyboard's cursor (KeyboardPlayMode.tsx): the card under it by its index in the hand as drawn
 * (-1 none), and while a card is aimed, the targets cycled: a battle (its index and attack) or the
 * transfer (battle -1), the one shown idx. */
static struct {
  int sel;
  int n_targets, idx;
  int8_t battle[MAX_BATTLES + 1];
  Card attack[MAX_BATTLES + 1];
} KB = {.sel = -1};
static int dragged_look(int cid, int *dim) { /* ActionButtons.tsx CardDiv: the dragged card's border and opacity */
  int it = DRAG.on && DRAG.index >= 0 && DRAG.index < hand_n && card_to_id(HAND[DRAG.index]) == cid;
  *dim = it && DRAG.moving && !DRAG.action;
  return it && DRAG.action;
}

static int selected(int cid) {
  for (int i = 0; i < nsel; i++) if (sel[i] == cid) return 1;
  return 0;
}

static int thin(const HandRect *r) { return r->w < 40 && r->h > 60; }
static double card_bottom(const HandRect *r) { return HL.rows.height - r->y - r->h; }

static cd_id hand_create(void *ctx, uint32_t i, uint32_t *data) {
  (void)ctx;
  Card c = HAND[i];
  const HandRect *r = &HL.slot[i];
  int cid = card_to_id(c), dim;
  int red = selected(cid) | dragged_look(cid, &dim);
  cd_id id = cd_id_new();
  face_open(c, thin(r));
  cd_attr(S_STYLE, face_style(c, FACE_HAND, r, card_bottom(r), red, 0));
  shown[cid].x = r->x; shown[cid].w = r->w; shown[cid].h = r->h; shown[cid].bottom = card_bottom(r);
  shown[cid].sel = (int8_t)red;
  shown[cid].dim = 0;
  /* the press starts a drag or a tap (DragContext.tsx startCardDrag): mouse and touch, as React */
  cd_listen(id, CD_EV_MOUSEDOWN);
  cd_listen(id, CD_EV_TOUCHSTART);
  cd_tag_set(id, TAG_CARD | (uint32_t)cid);
  cd_attr(ZS("data-card-index"), index_str(i));
  shown[cid].index = (int8_t)i;
  ATTR("data-location", "hand");
  cd_attr(ZS("data-player-id"), ZS("u-me-0000"));
  data_card(c);
  ATTR("draggable", "true");
  CLOSE();
  shown[cid].node = id;
  shown[cid].veil = 0;
  if (dim) cd_set_style(id, ZS("opacity"), ZS("0.3"));
  shown[cid].dim = (int8_t)dim;
  if (veiled(SHOW.veil_hand, cid)) veil_set(id, shown[cid].veil = 1);
  *data = (uint32_t)cid;
  return id;
}

/* A surviving card: only the properties that changed, one style property each. */
static void hand_patch(void *ctx, uint32_t i, cd_item *it) {
  (void)ctx;
  int cid = (int)it->data;
  const HandRect *r = &HL.slot[i];
  double bottom = card_bottom(r);
  int dim;
  int8_t sl = (int8_t)(selected(cid) | dragged_look(cid, &dim));
  if (r->x != shown[cid].x) cd_set_style_num(it->node, ZS("left"), shown[cid].x = r->x, CD_UNIT_PX);
  if (bottom != shown[cid].bottom) cd_set_style_num(it->node, ZS("bottom"), shown[cid].bottom = bottom, CD_UNIT_PX);
  if (r->w != shown[cid].w) cd_set_style_num(it->node, ZS("width"), shown[cid].w = r->w, CD_UNIT_PX);
  if (r->h != shown[cid].h) cd_set_style_num(it->node, ZS("height"), shown[cid].h = r->h, CD_UNIT_PX);
  if (sl != shown[cid].sel) cd_set_style(it->node, ZS("border-color"), (shown[cid].sel = sl) ? ZS("red") : ZS("black"));
  if (dim != shown[cid].dim) cd_set_style(it->node, ZS("opacity"), (shown[cid].dim = (int8_t)dim) ? ZS("0.3") : ZS("1"));
  if (shown[cid].index != (int8_t)i) cd_set_attr(it->node, ZS("data-card-index"), index_str((uint32_t)(shown[cid].index = (int8_t)i)));
  int8_t v = (int8_t)veiled(SHOW.veil_hand, cid);
  if (v != shown[cid].veil) veil_set(it->node, shown[cid].veil = v);
}

static void hand_destroy(void *ctx, cd_item *it) { (void)ctx; shown[it->data].node = 0; }
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
#define SEED_REPLAY 0.2 /* ReplayShare's frame */

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
/* getTextureStyle's wood: the colour, the picture (or the golden's stripes until it lands), the
 * tile's size and the seed's offset, appended to s. */
static void wood_add(SB *s, double seed, int end) {
  int32_t x, y;
  wood_offset(seed, &x, &y);
  sb_s(s, "background-color: rgb(195, 63, 8); background-image: var(");
  sb_s(s, end ? "--tex-wood-end" : "--tex-wood");
  sb_s(s, ", repeating-linear-gradient(90deg, transparent, "
          "transparent 1px, rgba(0, 0, 0, 0.1) 1px, rgba(0, 0, 0, 0.1) 2px)); background-size: 1920px 1080px; "
          "background-position: ");
  sb_i(s, x); sb_s(s, "px "); sb_i(s, y);
  sb_s(s, "px; background-repeat: repeat;");
}
static cd_str wood_style(double seed, int end) {
  static SB s;
  sb_reset(&s);
  wood_add(&s, seed, end);
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
  /* the cards it holds and, while a flight brings more, room for them at the end (handShape.tsx) */
  hand_layout(&WEB_HAND, V->my_hand_count + SHOW.arriving, width, &HL);
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
  hand_order();
  for (int i = 0; i < hand_n; i++)
    keys[i] = (uint32_t)card_to_id(HAND[i]) + 1 + (thin(&HL.slot[i]) ? 64u : 0u);
  cd_list_update(&hand, keys, (uint32_t)hand_n, &HAND_FNS);

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
  /* the coin (roleMotion.ts): the mark it is turning to, the face it turns from, when it started
   * (< 0: at rest), and the width it was last drawn at */
  int target, coin_from;
  double coin_t0, coin_delay, coin_scale;
  int departing, arriving; /* a ghost carries this seat's mark away, or brings one here */
} SeatUI;
static SeatUI seat_ui[MAX_PLAYERS];
static int n_seats;
static cd_id board, seats_anchor;

/* roleLedger.ts markWorn: the one mark a seat wears, from the roles shown (the defender, the
 * opener, the goods) and whether the table is open (showsSword: on an empty table only the opener
 * holds a sword); none for a seat that is out or a game that is not playing. */
static int worn(AnimRoles r, int table_open, const TableView *v, int seat) {
  if (v->seats[seat].status == PLAYER_STATUS_OUT || v->status != GAME_STATUS_PLAYING) return MARK_NONE;
  if ((r.good_mask >> seat) & 1) return MARK_CHECK;
  if (r.defender == seat) return MARK_SHIELD;
  if (table_open || seat == r.first_attacker) return seat == r.first_attacker ? MARK_LEAD : MARK_SWORD;
  return MARK_NONE;
}
static AnimRoles roles_of(const TableView *v) { return (AnimRoles){v->defender, v->first_attacker, (int)v->good_mask}; }
#ifdef TABLE_DEBUG
/* The mark a board's own roles give (the kernel's state, for the test build). */
static int mark_of(const TableView *v, int seat) { return worn(roles_of(v), v->num_battles > 0, v, seat); }
#endif

/* ---- what the badges are wearing (useRoleMotion.ts, the shown ledger) ------------------------
 * Not the board's roles: a run freezes the marks and moves them at the kernel's three moments
 * (anim_plan.h "the role beat"): a good being SET leads the stream (anim_goods_opening, as its
 * operation joins the run), a good CLEARED by a throw-in and the shield a transfer hands on turn
 * as that step opens (anim_goods_cleared, anim_pass_hand_off), and everything else at the closing
 * beat, once the run has landed (the board's own roles again). With no run the board's roles are
 * shown as they come (a bystander's write, anim_shown_ledger_allows). */
static struct { AnimRoles roles; int open, known, still; } SHOWN_ROLES; /* still: change in place, nothing flies */
static void roles_set(AnimRoles r, int open) {
  SHOWN_ROLES.roles = r;
  SHOWN_ROLES.open = open;
  SHOWN_ROLES.known = 1;
  SHOWN_ROLES.still = 0;
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
    u->mark = u->target = -1;
    u->coin_t0 = -1;
    u->coin_scale = 1;
    u->departing = u->arriving = 0;
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

/* A seat's mark drawn with a face (MARK_NONE: none). */
static void face_draw(SeatUI *u, int mark) {
  if (mark == u->mark) return;
  u->mark = mark;
  cd_clear(u->role);
  cd_set_attr(u->role, ZS("data-role-mark"), cd_internz(MARK_NAME[mark]));
  if (mark != MARK_NONE) { cd_at(u->role, 0); mark_emit(mark); }
}

/* THE COIN (roleMotion.ts coinFrameAt, ported value for value): a mark becoming another turns over
 * where it stands. Its width collapses as the cosine of a steady angle, waits a beat edge-on, the
 * face is swapped, and it opens as the sine; a mark that begins or ends makes one half. Every
 * duration is a proportion of the kernel's flight time (ANIM_TIME_MS). */
#define ROLE_FLIP_HALF_MS (ANIM_TIME_MS * 0.22)
#define ROLE_FLIP_SETTLE_MS (30 < ROLE_FLIP_HALF_MS * 0.3 ? 30 : ROLE_FLIP_HALF_MS * 0.3)
#define ROLE_FLIP_COLLAPSE_MS (ROLE_FLIP_HALF_MS - ROLE_FLIP_SETTLE_MS > 10 ? ROLE_FLIP_HALF_MS - ROLE_FLIP_SETTLE_MS : 10)
#define ROLE_EDGE_ON 0.001
enum { RAF_COIN = 4 };
static void kb_poll_start(void); /* the keyboard's cursor and arrow, polled after a draw */
static int coins_turning;

/* A seat's coin turns from one face to another (MARK_NONE: a half flip), the old face held for
 * `delay` ms first (roleMotion.ts RoleCoinPhase). */
static void coin_play(SeatUI *u, int from, int to, double delay) {
  u->coin_from = from;
  u->target = to;
  u->coin_t0 = cd_now();
  u->coin_delay = delay;
  if (!coins_turning) cd_raf_request(RAF_COIN);
  coins_turning = 1;
}

/* A seat's coin stands at a face, at once (the hand-off's swaps: RoleCoin.tsx stand). */
static void coin_stand(SeatUI *u, int mark) {
  u->coin_t0 = -1;
  u->target = mark;
  face_draw(u, mark);
  if (u->coin_scale != 1) { cd_set_style(u->role, ZS("transform"), ZS("scaleX(1)")); u->coin_scale = 1; }
}

/* One frame of a seat's coin; 1 while it is still turning. */
static int coin_frame(SeatUI *u, double now) {
  if (u->coin_t0 < 0) return 0;
  double t = now - u->coin_t0 - u->coin_delay, scale = 1;
  int face = u->target, from = u->coin_from == MARK_NONE ? -1 : u->coin_from, to = u->target == MARK_NONE ? -1 : u->target, turning = 1;
  if (t < 0) face = u->coin_from;
  else if (from >= 0 && t < ROLE_FLIP_HALF_MS) {
    face = from;
    scale = t < ROLE_FLIP_COLLAPSE_MS ? cos(t / ROLE_FLIP_COLLAPSE_MS * M_PI / 2) : ROLE_EDGE_ON;
  } else {
    if (from >= 0) t -= ROLE_FLIP_HALF_MS;
    if (to >= 0 && t < ROLE_FLIP_HALF_MS) scale = sin(t / ROLE_FLIP_HALF_MS * M_PI / 2);
    else { scale = 1; turning = 0; u->coin_t0 = -1; }
  }
  if (scale < ROLE_EDGE_ON) scale = ROLE_EDGE_ON;
  face_draw(u, face);
  if (scale != u->coin_scale) {
    static SB s;
    sb_reset(&s);
    sb_s(&s, "scaleX("); sb_num(&s, scale, CD_UNIT_NONE); sb_s(&s, ")");
    cd_set_style(u->role, ZS("transform"), sb_tmp(&s));
    u->coin_scale = scale;
  }
  return turning;
}

/* ---- the hand-off (roleLedger.ts roleFlightsBetween, RoleFlightsLayer.tsx) ----------------------
 * The shield and the opener's sword are the two marks that belong to a seat and then to another,
 * so they fly when they change hands: a ghost of the mark on a quadratic arc lifted toward the top
 * of the board (by up to 56 px), growing and casting a shadow as it lifts, spinning one whole
 * turn, over ROLE_FLIGHT_MS (0.8 of a card's flight) on the flights' curve, armed 25 ms after it is
 * drawn and stepped at most a 60 Hz frame at a time. The seat it leaves blanks at once (a pass's
 * old defender then turns its sword in a third of the way along); the seat it flies to turns its
 * own mark away to be edge-on as it lands, and stands the real mark up in the frame it lands. */
#define ROLE_FLIGHT_MS (ANIM_TIME_MS * 0.8)
#define ROLE_MAKE_WAY_DELAY_MS (ROLE_FLIGHT_MS - ROLE_FLIP_HALF_MS)
#define ROLE_PASS_SWORD_DELAY_MS (ROLE_FLIGHT_MS / 3)
#define ROLE_FRAME_CAP_MS (1000.0 / 60)
#define ROLE_FLIGHT_ARM_MS 25
static cd_region ghosts;
static struct { cd_id node; double fx, fy, tx, ty, elapsed, last; int to_seat; } ghost[2];
static int n_ghosts, ghosts_landed;
static uint32_t handoffs; /* for the tests */
static AnimRoles roles_drawn;
static int roles_drawn_known;

/* The cubic-bezier(0.25, 0.46, 0.45, 0.94) of time t (roleMotion.ts roleFlightEase: Newton on x). */
static double role_ease(double t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  double u = t;
  for (int i = 0; i < 8; i++) {
    double x = 3 * 0.25 * u * (1 - u) * (1 - u) + 3 * 0.45 * u * u * (1 - u) + u * u * u - t;
    double d = 3 * 0.25 * (1 - 4 * u + 3 * u * u) + 3 * 0.45 * (2 * u - 3 * u * u) + 3 * u * u;
    if (__builtin_fabs(d) < 1e-6) break;
    u -= x / d;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
  }
  return 3 * 0.46 * u * (1 - u) * (1 - u) + 3 * 0.94 * u * u * (1 - u) + u * u * u;
}

/* A ghost at progress p (eased): on the arc, grown and shadowed by its lift, turned spin * p. */
static void ghost_draw(int g, double p) {
  double dx = ghost[g].tx - ghost[g].fx, dy = ghost[g].ty - ghost[g].fy, dist = sqrt(dx * dx + dy * dy);
  double cx = (ghost[g].fx + ghost[g].tx) / 2, cy = (ghost[g].fy + ghost[g].ty) / 2 - (dist * 0.28 < 56 ? dist * 0.28 : 56);
  double q = 1 - p, lift = 1 - __builtin_fabs(p - 0.5) * 2;
  cd_set_style_num(ghost[g].node, ZS("left"), q * q * ghost[g].fx + 2 * q * p * cx + p * p * ghost[g].tx, CD_UNIT_PX);
  cd_set_style_num(ghost[g].node, ZS("top"), q * q * ghost[g].fy + 2 * q * p * cy + p * p * ghost[g].ty, CD_UNIT_PX);
  static SB s;
  sb_reset(&s);
  sb_s(&s, "translate(-50%, -50%) rotate("); sb_num(&s, 360 * p, CD_UNIT_DEG);
  sb_s(&s, ") scale("); sb_num(&s, 1 + 0.45 * lift, CD_UNIT_NONE); sb_s(&s, ")");
  cd_set_style(ghost[g].node, ZS("transform"), sb_tmp(&s));
  sb_reset(&s);
  sb_s(&s, "drop-shadow(0 "); sb_num(&s, 6 * lift, CD_UNIT_PX); sb_s(&s, " "); sb_num(&s, 8 * lift, CD_UNIT_PX);
  sb_s(&s, " rgba(0,0,0,"); sb_num(&s, 0.45 * lift, CD_UNIT_NONE); sb_s(&s, "))");
  cd_set_style(ghost[g].node, ZS("filter"), sb_tmp(&s));
}

/* The roles drawn change from `was` to `now`: each mark that changed hands flies (both seats must
 * be on screen), and the two seats of each flight are told. */
static void handoff_start(AnimRoles was, AnimRoles now) {
  int from[2], to[2], kind[2], n = 0;
  if (was.defender != now.defender && was.defender >= 0 && now.defender >= 0 && was.defender < n_seats && now.defender < n_seats) {
    from[n] = was.defender; to[n] = now.defender; kind[n++] = MARK_SHIELD;
  }
  if (was.first_attacker != now.first_attacker && was.first_attacker >= 0 && now.first_attacker >= 0
      && was.first_attacker < n_seats && now.first_attacker < n_seats) {
    from[n] = was.first_attacker; to[n] = now.first_attacker; kind[n++] = MARK_LEAD;
  }
  if (!n) return;
  cd_region_hide(&ghosts); /* a hand-off still in the air is overtaken: its seats stand */
  for (int g = 0; g < n_ghosts; g++) seat_ui[ghost[g].to_seat].arriving = 0;
  for (int k = 0; k < n_seats; k++) seat_ui[k].departing = 0;
  cd_region_show(&ghosts, 1);
  cd_at(container, 0);
  OPEN("div"); cd_name_open(ghosts.root);
  STYLE("position: fixed; top: 0px; left: 0px; width: 100%; height: 100%; pointer-events: none; z-index: 10001; user-select: none;");
  for (int g = 0; g < n; g++) {
    double fx, fy, tx, ty;
    centre_of(seat_ui[from[g]].role, &fx, &fy);
    centre_of(seat_ui[to[g]].role, &tx, &ty);
    ghost[g] = (typeof(ghost[g])){cd_id_new(), fx, fy, tx, ty, 0, -1, to[g]};
    OPEN("div"); cd_name_open(ghost[g].node);
    cd_attr(ZS("data-role-flight"), kind[g] == MARK_SHIELD ? ZS("shield") : ZS("sword"));
    static SB st;
    sb_reset(&st);
    sb_s(&st, "position: absolute; "); sb_px(&st, "left: ", fx); sb_px(&st, "top: ", fy);
    sb_s(&st, "transform: translate(-50%, -50%); line-height: 0; will-change: transform, left, top;");
    cd_attr(S_STYLE, sb_tmp(&st));
    mark_emit(kind[g]);
    CLOSE();
  }
  CLOSE();
  cd_region_end(&ghosts);
  n_ghosts = n;
  ghosts_landed = 0;
  handoffs++;
  int coming[MAX_PLAYERS] = {0}; /* the kind flying to each seat */
  for (int g = 0; g < n; g++) { seat_ui[from[g]].departing = 1; seat_ui[to[g]].arriving = 1; coming[to[g]] = kind[g]; }
  for (int k = 0; k < n_seats; k++) { /* RoleCoin.tsx advance, the seat's two flags read together */
    SeatUI *u = &seat_ui[k];
    if (u->departing) { /* the ghost is this seat's mark now: blank at once; a pass's old defender turns its sword in */
      int t = u->arriving ? MARK_NONE : worn(now, SHOWN_ROLES.open, V, k);
      coin_stand(u, MARK_NONE);
      if (t != MARK_NONE) coin_play(u, MARK_NONE, t, ROLE_PASS_SWORD_DELAY_MS);
    } else if (u->arriving) { /* what it wears turns away, edge-on as the ghost lands */
      int wearing = u->coin_t0 >= 0 ? (u->target == coming[k] ? u->coin_from : u->target) : u->mark;
      coin_stand(u, wearing);
      if (wearing != MARK_NONE) coin_play(u, wearing, MARK_NONE, ROLE_MAKE_WAY_DELAY_MS);
    }
  }
  if (!coins_turning) cd_raf_request(RAF_COIN);
  coins_turning = 1;
}

/* One frame of the ghosts; 1 while one is still in the air. When all have landed they go, and the
 * seats they flew to stand their marks up in the same frame. */
static int ghosts_frame(double now) {
  if (!n_ghosts) return 0;
  for (int g = 0; g < n_ghosts; g++) {
    if (ghost[g].last >= 0) {
      double dt = now - ghost[g].last;
      ghost[g].elapsed += dt < 0 ? 0 : dt > ROLE_FRAME_CAP_MS ? ROLE_FRAME_CAP_MS : dt;
    }
    ghost[g].last = now;
    double since = ghost[g].elapsed - ROLE_FLIGHT_ARM_MS, p = since <= 0 ? 0 : since / ROLE_FLIGHT_MS;
    ghost_draw(g, role_ease(p > 1 ? 1 : p));
    if (p >= 1 && !(ghosts_landed >> g & 1)) ghosts_landed |= 1 << g;
  }
  if (ghosts_landed != (1 << n_ghosts) - 1) return 1;
  cd_region_hide(&ghosts);
  for (int g = 0; g < n_ghosts; g++) {
    SeatUI *a = &seat_ui[ghost[g].to_seat];
    a->arriving = 0;
    coin_stand(a, worn(SHOWN_ROLES.roles, SHOWN_ROLES.open, V, ghost[g].to_seat));
  }
  for (int k = 0; k < n_seats; k++) seat_ui[k].departing = 0;
  n_ghosts = 0;
  return 0;
}

static void coins_frame(void) {
  double now = cd_now();
  int any = ghosts_frame(now);
  for (int s = 0; s < n_seats; s++) any |= coin_frame(&seat_ui[s], now);
  coins_turning = any;
  if (any) cd_raf_request(RAF_COIN);
}

static void render_seats(void) {
  if (n_seats != V->num_players) { seats_build(); roles_drawn_known = 0; }
  if (SHOWN_ROLES.known) {
    AnimRoles r = SHOWN_ROLES.roles;
    if (roles_drawn_known && !SHOWN_ROLES.still && (r.defender != roles_drawn.defender || r.first_attacker != roles_drawn.first_attacker))
      handoff_start(roles_drawn, r);
    roles_drawn = r;
    roles_drawn_known = 1;
  }
  for (int s = 0; s < n_seats; s++) {
    SeatUI *u = &seat_ui[s];
    if (seat_lift(s) != u->lift) { u->lift = seat_lift(s); cd_set_style(u->seat.root, ZS("transform"), seat_transform(s)); }
    int mark = worn(SHOWN_ROLES.roles, SHOWN_ROLES.open, V, s);
    if (u->mark < 0) { u->target = mark; face_draw(u, mark); } /* a new ring: drawn as it is */
    else if (!u->arriving && !u->departing && mark != u->target) {
      /* nothing flies here: the ordinary gesture between two marks (resolveGesture) */
      int resting = u->coin_t0 >= 0 ? u->coin_from : u->mark;
      if (resting == mark) coin_stand(u, mark); /* the mark came back: put it back */
      else coin_play(u, resting, mark, 0);
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

static cd_region end_screen; /* root mode: the page that takes the table's place */

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

/* WinScreen.tsx: when the game is over the table goes and the page takes its place (GameView routes
 * GAME_OVER to it): the wool, the title, the results on one plank, ReplayShare, and the button. The
 * table is kept, hidden, for the next deal. ReplayShare shows its frame as React does for a game
 * with no replay snapshot (the port has no server to keep one); the button is Play again (there is
 * no lobby to continue to); the game is not rated, so no rating shows. */
static int end_shown;
static void render_end(void) {
  int over = V->status == GAME_STATUS_GAME_OVER;
  if (over != end_shown) cd_set_style(container, ZS("display"), (end_shown = over) ? ZS("none") : ZS(""));
  if (!cd_region_show(&end_screen, over)) return;
  cd_at(CD_MOUNT, 0);
  OPEN("div");
  cd_name_open(end_screen.root);
  ATTR("data-end", "true");
  CLASS("page"); STYLE("padding: 0.25rem 1rem 1rem;");
  OPEN("div"); CLASS("bg-wool"); STYLE("background-image: var(--tex-wool);"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--default"); CLOSE();
  OPEN("div"); CLASS("bg-soviet"); CLOSE();
  OPEN("div"); CLASS("bg-vignette bg-vignette--soviet"); CLOSE();
  OPEN("h1"); CLASS("win-screen__title"); TEXT("done - Two of us"); CLOSE(); /* "done - " and the table's title */
  OPEN("div"); CLASS("win-screen__results");
  OPEN("div"); CLASS("bg-wood"); cd_attr(S_STYLE, wood_style(SEED_RESULTS, 1)); CLOSE();
  /* The finish order is the kernel's (anim_finish_rows: everyone out, first out first, then the fool). */
  AnimFinishRow rows[MAX_PLAYERS + 1];
  int n = anim_finish_rows((const unsigned char *)V->elimination, V->num_eliminated, V->fool, V->num_players, V->my_seat, rows, MAX_PLAYERS + 1);
  for (int place = 1; place <= n; place++)
    for (int i = 0; i < n; i++) if (rows[i].place == place) result_row(place, n, rows[i].seat);
  CLOSE();
  OPEN("div"); CLASS("replay-share");
  STYLE("position: relative; z-index: var(--z-content); display: flex; flex-direction: column; align-items: center; gap: 0.6rem; margin: 0px 0px 0.5rem;");
  OPEN("div");
  { /* the QR's wood frame (getTextureStyle(woodUrl, false, 0.2)) */
    static SB fs;
    sb_reset(&fs);
    sb_s(&fs, "width: 188px; height: 188px; box-sizing: border-box; display: flex; align-items: center; justify-content: center; "
              "padding: 12px; border-radius: 10px; border: 2px solid rgb(93, 58, 26); box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), "
              "inset 0 -1px 0 rgba(0,0,0,0.3), 0 3px 6px rgba(0,0,0,0.4); ");
    wood_add(&fs, SEED_REPLAY, 1);
    sb_s(&fs, " line-height: 0;");
    cd_attr(S_STYLE, sb_tmp(&fs));
  }
  OPEN("span"); CLASS("text-shadow");
  STYLE("color: var(--color-text-primary); font-size: 0.8rem; text-align: center; line-height: 1.35; opacity: 0.85;");
  OPEN("span"); TEXT("Replay code unavailable for this game"); CLOSE();
  CLOSE();
  CLOSE();
  OPEN("div");
  STYLE("min-height: 1.5rem; width: 100%; display: flex; flex-direction: row; flex-wrap: wrap; align-items: center; justify-content: center; gap: 0.9rem;");
  CLOSE();
  CLOSE();
  cd_id again = cd_id_new();
  OPEN("button"); cd_listen(again, CD_EV_CLICK); cd_tag_set(again, TAG_AGAIN);
  CLASS("btn-wood btn-wood--lg"); cd_attr(S_STYLE, wood_style(SEED_AGAIN, 1));
  OPEN("span"); CLASS("btn-wood-text"); OPEN("span"); TEXT("Play again"); CLOSE(); CLOSE();
  CLOSE();
  CLOSE();
  cd_region_end(&end_screen); /* the button's id goes with the screen */
}

/* ---- the animations (README, Animations) ----------------------------------------------------
 * Every operation on the game (a move of mine, a bot step, a deal) is captured as the kernel's
 * stream for my seat (anim_run.c) and appended to one growing run, as the React table appends each
 * push (useAnimationRun.ts): the steps already there keep their timing, nothing is spliced or
 * cancelled. The plan is the kernel's (client_plan_step, client_plan): when each step opens and
 * lands, which steps fly together (a beat), for how long, and the stock's cards in the air. Every
 * render asks it where the run stands now (anim_plan_at) and draws that: the board of the last step
 * to land (every step a skipped frame passed landed in order), the beat in the air flying, the
 * cards it carries hidden where they left and where they land. A frame loop renders while a run
 * plays. When the last step has landed the run ends and the game's own board is drawn: it is the
 * stream's final board, with the roles the closing beat hands on (anim_run.h). */
enum { TIMER_TEX = 2, TIMER_BOT_GO = 3, TIMER_SAVE_RETRY = 4, TIMER_BOT_BASE = 16 }; /* the bot's: TIMER_BOT_BASE + its generation */
#define TIMER_ARM 0x80000000u /* a flight's arm: TIMER_ARM | its generation */
enum { RAF_SLICE = 0, RAF_PAINTED = 1, RAF_PAINTED2 = 2, RAF_ANIM = 3 };
enum { RUN_ROOM = MAX_SNAPS }; /* the most steps one operation adds (one per hook) */

static RunStep run[ANIM_MAX_STEPS];
static TableView run_pre;      /* what shows until step 0 lands: the board the run started on */
static AnimPlanAsk ASK;
static AnimPlan PLAN;
static AnimFrame FR;           /* where the run stood at the last render */
static int run_n, run_landed, run_opened;
static double run_origin = -1; /* page ms when the run started; < 0: no run */
static double run_settles;     /* page ms when the run's last step lands (kept after it has) */
static double anim_speed = 1;  /* the test build's: the run's clock goes this many times faster */
static uint32_t runs_started;  /* for the tests */

static int run_on(void) { return run_origin >= 0; }
/* A move of mine that is the whole operation (an attack, a pass, a cover, a pickup and nothing the
 * server adds to it) is the board at once once its flight is off, as React's is: its push holds no
 * event the table has not already flown, so React adopts the board when the server answers
 * (AnimationContext's "all events optimistic: just update state"), not when the card lands; a
 * server here answers in no time. Steps [0, run_ahead) show their board from then on: the count on
 * my seat, the hand laid out without the card and the pills are the move's at once, the card
 * itself hidden where it lands until it does. run_ahead_at: the step whose opening sets it. */
static int run_ahead, run_ahead_at = -1, run_ahead_to;
static const TableView *run_board(void) {
  int k = run_landed > run_ahead ? run_landed : run_ahead;
  return k ? &run[k - 1].board : &run_pre;
}

/* The plan of the whole run, steps [from, run_n) newly added: the kernel's, from each step's event
 * and board (as pushSequence.ts and animPlan.ts planFor hand them over). */
static int run_plan(int from) {
  for (int i = from; i < run_n; i++) {
    const PushEvent *e = &run[i].ev;
    ClientPlanStep st;
    __builtin_memset(&st, 0, sizeof st);
    st.type = e->type;
    st.seat = e->seat;
    st.from = (uint8_t)e->from; /* -1, none, is ANIM_LOC_NONE */
    st.to = (uint8_t)e->to;
    st.n_cards = e->n_cards;
    for (int k = 0; k < e->n_cards; k++) st.cards[k] = e->cards[k];
    if (client_plan_step(&ASK, i, &st, &run[i].board) != ANIM_EOK) return 0;
  }
  return client_plan(&ASK, run_n, &run[run_n - 1].board, &PLAN) == ANIM_EOK;
}

static void flights_clear(void);
static TableView run_final; /* the board the run settles on: the last operation's final board */

/* The run is over: the closing beat is the bystander's write the next render makes (the board's
 * own roles, now nothing is playing). */
static void roles_settle(void) {}

/* Step k opened: a good it clears turns with its card, and a transfer hands the shield on with
 * it (useAnimationRun's onOpened in AnimationContext.tsx, the kernel's two rules). */
static void roles_opened(int k) {
  const PushEvent *e = &run[k].ev;
  AnimRoles out;
  if (!SHOWN_ROLES.known) return;
  if (e->to == EVW_LOC_TABLE && anim_goods_cleared(SHOWN_ROLES.roles, (int)run[k].board.good_mask, &out)) roles_set(out, 1);
  if (e->type == EVW_T_ATTACK_PASS && e->seat >= 0
      && anim_pass_hand_off(SHOWN_ROLES.roles, 1u << e->seat, run_final.defender, &out)) roles_set(out, 1);
}

/* The run is over: every step has landed (or it is landed at once: no room, a plan refused). */
static void run_end(void) {
  run_origin = -1;
  run_n = run_landed = run_opened = 0;
  run_ahead = run_ahead_to = 0;
  run_ahead_at = -1;
  __builtin_memset(&FR, 0, sizeof FR);
  FR.step = ANIM_STEP_NONE;
  flights_clear();
  roles_settle();
}

/* Before an operation: its hooks are captured from here. A run with no room for one more operation
 * is landed first (a guard: a deal is 16 steps, a move a handful, the run holds 128). */
static void op_begin(void) {
  if (run_on() && run_n > ANIM_MAX_STEPS - RUN_ROOM) run_end();
  run_capture_begin(&G);
}

/* After it: its steps join the run, which starts now if none was playing, on the board on screen
 * (a deal's run on the empty table its first step carries). `actor` the seat that acted, -1 none. */
static void op_end(int actor) {
  int was = run_on(), from = run_n;
  if (!was && V) run_pre = *V; /* before the capture writes into run[], which V may still point into */
  int n = run_capture_end(&G, ME, actor, G.status == GAME_STATUS_GAME_OVER, run + run_n, ANIM_MAX_STEPS - run_n);
  if (n <= 0) return; /* nothing shown, or a stream that does not read: the board is drawn as it is */
  if (!was && n > 1 && run[1].ev.type == EVW_T_DEAL) run_pre = run[0].board; /* a deal lands on the empty table */
  run_final = *run_capture_final();
  AnimRoles opening; /* a good this operation sets leads it */
  if (SHOWN_ROLES.known && anim_goods_opening(SHOWN_ROLES.roles, (int)run[run_n].board.good_mask, &opening))
    roles_set(opening, SHOWN_ROLES.open);
  int mine_alone = actor == ME;
  for (int i = from; i < from + n && mine_alone; i++) {
    const PushEvent *e = &run[i].ev;
    mine_alone = e->seat == ME && (e->type == EVW_T_ATTACK_PASS || e->type == EVW_T_COVER || e->type == EVW_T_PICKUP);
  }
  if (mine_alone) { run_ahead_at = from; run_ahead_to = from + n; }
  run_n += n;
  if (!run_plan(from)) { run_end(); return; }
  if (!was) {
    run_origin = cd_now();
    run_landed = run_opened = 0;
    row_ms = 0;
    runs_started++;
    cd_raf_request(RAF_ANIM);
  }
  run_settles = run_origin + PLAN.total_ms / anim_speed;
}

/* Where the run stands now: the steps landed since the last render (in order), the steps opened
 * (their role beats turn now), and the run's end. */
static void run_sample(void) {
  int t = (int)((cd_now() - run_origin) * anim_speed);
  if (anim_plan_at(&PLAN, t, &FR) != ANIM_EOK) { run_end(); return; }
  if (FR.landed > run_landed) run_landed = FR.landed < run_n ? FR.landed : run_n;
  while (run_opened < run_n && PLAN.steps[run_opened].start_ms <= t) {
    row_ms = PLAN.steps[run_opened].duration_ms; /* the row moves with the card, over its time */
    roles_opened(run_opened);
    run_opened++;
  }
  if (FR.done && run_landed >= run_n) run_end();
}

/* What the beat in the air shows besides the landed board (SHOW). */
static void run_show(void) {
  __builtin_memset(&SHOW, 0, sizeof SHOW);
  RV = V;
  if (!run_on()) return;
  SHOW.from_deck = FR.in_flight_from_deck;
  SHOW.to_flipped = FR.in_flight_to_flipped;
  SHOW.trump_out = !card_is_none(FR.trump_flight);
  if (FR.step == ANIM_STEP_NONE) return;
  const AnimPlanStep *ps = &PLAN.steps[FR.step];
  SHOW.flight_ms = ps->duration_ms;
  for (int k = ps->beat_first; k < ps->beat_first + ps->beat_n; k++) {
    const PushEvent *e = &run[k].ev;
    uint64_t ids = 0;
    for (int i = 0; i < e->n_cards; i++) if (e->cards[i].suit >= 0) ids |= 1ull << card_to_id(e->cards[i]);
    /* ahead: the board drawn holds this step's cards already (run_ahead: a lone move of mine), so
     * they are hidden where they land, as React hides its own move's cards on the board it adopted */
    int ahead = k < run_ahead;
    if (e->from == EVW_LOC_HAND && e->seat == ME) SHOW.veil_hand |= ids;
    if (e->from == EVW_LOC_TABLE) SHOW.veil_table |= ids;
    if (e->to == EVW_LOC_HAND && e->seat == ME) { if (ahead) SHOW.veil_hand |= ids; else SHOW.arriving += e->n_cards; }
    if (e->to == EVW_LOC_TABLE && e->type == EVW_T_COVER && ahead) SHOW.veil_table |= ids;
    if (e->to == EVW_LOC_TABLE && e->type == EVW_T_ATTACK_PASS) {
      SHOW.veil_table |= ids;
      RV = &run[k].board; /* the row makes room for the pile now (React's arrivingPiles) */
    }
    if (e->type == EVW_T_COVER && e->has_target) SHOW.covering |= 1ull << card_to_id(e->target);
  }
}

/* ---- the flights (FlightCard.tsx, flightGeometry.ts) ---------------------------------------------
 * The beat in the air's cards, each one node in an overlay (AnimationOverlay.tsx: fixed, over
 * everything, never hit), made when the beat opens and gone when it lands. A flight is put at the
 * centre of where its card is drawn, at scale 1.5 and the tilt it lies at there, and FLIGHT_ARM_MS
 * later given its landing: the centre of where the card will be drawn, scale 1.8, the tilt it will
 * lie at, and a shadow, with a CSS transition of the beat's duration on the flights' curve. The
 * browser moves it; every endpoint is measured once, as React measures in a layout effect. */
enum { MAX_FLIGHTS = 64, CARD_H = 70 };
static cd_region overlay;
static struct { cd_id node, face; double x, y, angle; int cid; } flight[MAX_FLIGHTS]; /* cid: the card, -1 a stack of backs */
static int n_flights, flight_beat = -1, flight_ms, flights_made;
static uint32_t flight_gen; /* the arm's generation: an older beat's timer arms nothing */

static void flights_clear(void) {
  cd_region_hide(&overlay);
  n_flights = 0;
  flight_beat = -1;
  flight_gen++;
}

/* The transform a flight is drawn with: hung by its centre, turned about its bottom edge (so the
 * pivot is paid back), grown and tilted (FlightCard.tsx). */
static void sb_flight_transform(SB *s, double scale, double angle) {
  sb_s(s, "translate(-50%, -50%) translate(");
  sb_num(s, -scale * (CARD_H / 2.0) * sin(angle), CD_UNIT_PX);
  sb_s(s, ", ");
  sb_num(s, (scale * cos(angle) - 1) * (CARD_H / 2.0), CD_UNIT_PX);
  sb_s(s, ") scale(");
  sb_num(s, scale, CD_UNIT_NONE);
  sb_s(s, ") rotate(");
  sb_num(s, angle, CD_UNIT_RAD);
  sb_s(s, ")");
}

/* A flying face: CardFace's own style with the overlay's border, shadow (none until armed), filter
 * and colour (FlightCard.tsx NORMAL). */
#define FLIGHT_FACE "background-color: var(--color-card-face); width: 50px; height: 70px; border-radius: 5px; " \
  "border: 2px solid black; box-shadow: 0 0px 0px rgba(0,0,0,0.4); cursor: default; user-select: none; " \
  "-webkit-user-select: none; -webkit-touch-callout: none; pointer-events: none; " \
  "transition: transform 0.2s ease-in-out, opacity 0.2s ease-in-out, box-shadow 0.2s ease-in-out; position: relative; " \
  "font-family: Georgia, serif; font-weight: bold; display: flex; flex-direction: column; font-size: 20px; " \
  "line-height: 15px; filter: none; color: "

/* One flight: a face of c, or a stack of `backs` card backs (another seat's draw: CardBack with as
 * many layers), from (x0, y0) at angle a0 to (x1, y1) at a1. */
static void flight_add(Card c, int backs, double x0, double y0, double a0, double x1, double y1, double a1) {
  if (n_flights == MAX_FLIGHTS) return;
  cd_id node = cd_id_new(), face = 0;
  OPEN("div");
  cd_name_open(node);
  static SB s;
  sb_reset(&s);
  sb_s(&s, "position: absolute; ");
  sb_px(&s, "left: ", x0);
  sb_px(&s, "top: ", y0);
  sb_s(&s, "transform-origin: center bottom; transform: ");
  sb_flight_transform(&s, 1.5, a0);
  sb_s(&s, "; opacity: 1; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; transition: none;");
  cd_attr(S_STYLE, sb_tmp(&s));
  if (backs) {
    OPEN("div"); STYLE("position: relative; width: 50px; height: 70px;");
    for (int i = 0; i < backs; i++) { OPEN("div"); cd_attr(S_STYLE, back_style((uint32_t)i, 0, 1)); CLOSE(); }
    CLOSE();
  } else {
    face_open(c, 0);
    cd_attr(S_STYLE, card_red(c) ? ZS(FLIGHT_FACE "rgb(220, 38, 38);") : ZS(FLIGHT_FACE "rgb(0, 0, 0);"));
    cd_name_open(face = cd_id_new());
    CLOSE();
  }
  CLOSE();
  flight[n_flights].node = node;
  flight[n_flights].face = face;
  flight[n_flights].x = x1;
  flight[n_flights].y = y1;
  flight[n_flights].angle = a1;
  flight[n_flights].cid = backs ? -1 : card_to_id(c);
  n_flights++;
}

/* The flights' landings, FLIGHT_ARM_MS after they were drawn at their start (one crossed paint). */
static void flights_arm(uint32_t gen) {
  if (gen != (flight_gen & 0x7fffffffu)) return;
  for (int i = 0; i < n_flights; i++) {
    static SB s;
    sb_reset(&s);
    sb_s(&s, "left "); sb_i(&s, flight_ms); sb_s(&s, "ms " EASE ", top "); sb_i(&s, flight_ms);
    sb_s(&s, "ms " EASE ", transform "); sb_i(&s, flight_ms); sb_s(&s, "ms " EASE);
    cd_set_style(flight[i].node, ZS("transition"), sb_tmp(&s));
    cd_set_style_num(flight[i].node, ZS("left"), flight[i].x, CD_UNIT_PX);
    cd_set_style_num(flight[i].node, ZS("top"), flight[i].y, CD_UNIT_PX);
    sb_reset(&s);
    sb_flight_transform(&s, 1.8, flight[i].angle);
    cd_set_style(flight[i].node, ZS("transform"), sb_tmp(&s));
    if (flight[i].face) cd_set_style(flight[i].face, ZS("box-shadow"), ZS("0 10px 20px rgba(0,0,0,0.4)"));
  }
}


/* A card on the table: its face and the tilt it lies at (a cover across, the attack under it the
 * other way: flightGeometry.ts tableTilt); 0 when the row does not hold it. */
static cd_id table_face(int cid, double *angle) {
  for (int i = 0; i < RV->num_battles; i++) {
    const Battle *b = &RV->battles[i];
    int aid = card_to_id(b->attack), covered = b->defense.suit >= 0;
    if (aid == cid) { *angle = covered ? -COVER_RAD : 0; return battle_attack[aid]; }
    if (covered && card_to_id(b->defense) == cid) { *angle = COVER_RAD; return battle_defense[aid]; }
  }
  *angle = 0;
  return 0;
}

/* How far a table card's cell is from where it comes to rest (a glide still running): a landing is
 * aimed at the rest (flightGeometry.ts restingCentre). */
static void glide_offset(int aid, double *dx, double *dy) {
  double r[4];
  *dx = *dy = 0;
  if (!battle_cell[aid] || cd_now() >= cell_glide_end[aid]) return; /* at rest: its box is where it is */
  cd_rect(battle_cell[aid], r);
  *dx = r[0] - cell_rest[aid][0];
  *dy = r[1] - cell_rest[aid][1];
}

static cd_id seat_hand(int seat) { return seat_ui[seat].hand; } /* another seat's mini hand: where its cards come and go */
static cd_id hand_plane_id(void) { return hand_plane; }

/* Where a card of step k lands in MY hand: the kernel's slot for it in the hand that step leaves,
 * off the plane's bottom edge (handShape.tsx handLandingCentre). */
static int my_hand_landing(int k, Card c, double *x, double *y) {
  const TableView *b = &run[k].board;
  int at = -1;
  for (int i = 0; i < b->my_hand_count; i++) if (b->my_hand[i].suit == c.suit && b->my_hand[i].value == c.value) at = i;
  if (at < 0) return 0;
  cd_viewport_rec vp;
  cd_viewport(&vp);
  static HandLayout l;
  hand_layout(&WEB_HAND, b->my_hand_count, (double)vp.w - 20, &l);
  double r[4];
  cd_rect(hand_plane_id(), r);
  const HandRect *s = &l.slot[at];
  *x = r[0] + s->x + s->w / 2;
  *y = r[1] + r[3] - (l.rows.height - s->y - s->h) - s->h / 2;
  return 1;
}

/* The flights of the beat opening at `beat`: each card of each of its steps, from where it is drawn
 * to where it will be (flightGeometry.ts planFlights). Measured now, once. */
static void flights_open(int beat) {
  const AnimPlanStep *ps = &PLAN.steps[beat];
  flight_ms = ps->duration_ms;
  cd_viewport_rec vp;
  cd_viewport(&vp);
  int made = 0;
  for (int k = beat; k < beat + ps->beat_n; k++) {
    const PushEvent *e = &run[k].ev;
    const AnimPlanStep *st = &PLAN.steps[k];
    int n = e->n_cards;
    if (n == 0 || e->type == EVW_T_MAGIC_TRANSITION) continue;
    int trump = !card_is_none(st->trump_out) && st->trump_from == ANIM_LOC_FLIPPED; /* the draw's last card is the trump */
    int backs = 0;
    for (int i = 0; i < n; i++) backs += e->cards[i].suit < 0;
    if (!made) { cd_region_show(&overlay, 1); cd_at(container, 0); OPEN("div"); cd_name_open(overlay.root);
      STYLE("position: fixed; top: 0px; left: 0px; width: 100%; height: 100%; pointer-events: none; z-index: 10000; "
            "user-select: none; -webkit-user-select: none; -webkit-touch-callout: none;");
      made = 1; }
    if (backs == n) { /* another seat's draw: one stack of backs out of the stock, and the trump face up out of its slot */
      double x1, y1, x0, y0;
      centre_of(seat_hand(e->seat), &x1, &y1);
      if (n - trump > 0 && deck_spot) { centre_of(deck_spot, &x0, &y0); flight_add(e->cards[0], n - trump, x0, y0, 0, x1, y1, 0); }
      if (trump && flipped_slot) { centre_of(flipped_slot, &x0, &y0); flight_add(st->trump_out, 0, x0, y0, 0, x1, y1, 0); }
      continue;
    }
    for (int i = 0; i < n; i++) {
      Card c = e->cards[i];
      int cid = card_to_id(c), precise = 0;
      double x0 = vp.w / 2.0, y0 = vp.h - 100.0, a0 = 0, x1 = vp.w / 2.0, y1 = vp.h / 2.0, a1 = 0;
      /* where it leaves from */
      if (trump && i == n - 1) { if (!flipped_slot) continue; centre_of(flipped_slot, &x0, &y0); }
      else if (e->from == EVW_LOC_DECK) { if (!deck_spot) continue; centre_of(deck_spot, &x0, &y0); }
      else if (e->from == EVW_LOC_FLIPPED) { if (!flipped_slot) continue; centre_of(flipped_slot, &x0, &y0); }
      else if (e->from == EVW_LOC_HAND) {
        if (e->seat == ME && shown[cid].node) centre_of(shown[cid].node, &x0, &y0);
        else if (e->seat != ME) centre_of(seat_hand(e->seat), &x0, &y0);
      } else if (e->from == EVW_LOC_TABLE) {
        cd_id f = table_face(cid, &a0);
        if (f) centre_of(f, &x0, &y0); else x0 = vp.w / 2.0, y0 = vp.h / 2.0;
      }
      /* where it lands */
      if (e->to == EVW_LOC_TABLE && e->type == EVW_T_COVER && e->has_target && battle_attack[card_to_id(e->target)]) {
        int tid = card_to_id(e->target);
        double r[4], dx, dy;
        cd_rect(battle_attack[tid], r); /* upright: its turn waits for the arm */
        glide_offset(tid, &dx, &dy);
        x1 = r[0] - dx + r[2] / 2 + sin(COVER_RAD) * (r[3] / 2); /* the attack's centre turned about its bottom edge */
        y1 = r[1] + r[3] - dy - cos(COVER_RAD) * (r[3] / 2);
        a1 = COVER_RAD;
        precise = 1;
      } else if (e->to == EVW_LOC_TABLE && battle_attack[cid]) { /* the cell the row has made for it */
        double dx, dy;
        centre_of(battle_attack[cid], &x1, &y1);
        glide_offset(cid, &dx, &dy);
        x1 -= dx; y1 -= dy;
        precise = 1;
      } else if (e->to == EVW_LOC_HAND && e->seat == ME) {
        precise = my_hand_landing(k, c, &x1, &y1);
      } else if (e->to == EVW_LOC_HAND) {
        centre_of(seat_hand(e->seat), &x1, &y1);
      } else if (e->to == EVW_LOC_DISCARD) {
        centre_of(discard.parent, &x1, &y1);
      } else if (e->to == EVW_LOC_FLIPPED) {
        if (!flipped_slot) continue;
        centre_of(flipped_slot, &x1, &y1);
        precise = 1;
      }
      if (!precise) { x1 += i * 3; y1 += i * 3; } /* cards into one unmeasured place do not stack exactly */
      flight_add(c, 0, x0, y0, a0, x1, y1, a1);
    }
  }
  if (!made) return;
  CLOSE();
  cd_region_end(&overlay);
  flights_made += n_flights;
  cd_timer_start(FLIGHT_ARM_MS, TIMER_ARM | (flight_gen & 0x7fffffffu));
}

/* After a render: the beat in the air flies (made once, as it opens), and is gone once it lands. */
static void flights_update(void) {
  int beat = run_on() && FR.step != ANIM_STEP_NONE ? PLAN.steps[FR.step].beat_first : -1;
  if (beat == flight_beat) return;
  flights_clear();
  if (beat < 0) return;
  flight_beat = beat;
  flights_open(beat);
}

/* ---- one render ----------------------------------------------------------- */

static cd_str turn_shown;
static int drag_ask(void);
static void render_drag(void);
static void render_keys(void);

/* The board on screen: the run's (where it stands now) while one plays, else the game's. */
static void render_board(void) {
  if (run_on()) V = run_board();
  else {
    if (client_adopt_board(&CT, &G, ME) != CLIENT_OK) __builtin_trap();
    V = &CT.view;
  }
  run_show();
  if (anim_shown_ledger_allows(ANIM_CLAIM_BYSTANDER, run_on())) {
    roles_set(roles_of(V), V->num_battles > 0);
    SHOWN_ROLES.still = V->status == GAME_STATUS_GAME_OVER; /* a finished board's marks fly nowhere (syncFromView) */
  }
  client_view_rules(V, SHOW.from_deck, SHOW.to_flipped, SHOW.trump_out, &R);
  drop_now = 0;
  if (DRAG.on && DRAG.action) { DRAG.asked = 0; drag_ask(); drop_now = &DRAG.play; } /* asked again on the board now drawn */
  render_deck();
  render_discard();
  render_hand();
  render_table();
  render_seats();
  render_end();
  render_drag();
  render_keys();
  kb_poll_start();
  cd_str turn = V->status == GAME_STATUS_GAME_OVER ? ZS("over") : bot_pending ? ZS("bot") : ZS("human");
  if (turn != turn_shown) cd_set_attr(container, ZS("data-turn"), turn_shown = turn);
  flights_update();
  /* the move's flight is off, measured from where its card was drawn: the board is the move's now */
  if (run_on() && run_ahead_at >= 0 && run_opened > run_ahead_at) {
    run_ahead = run_ahead_to;
    run_ahead_at = -1;
    render_board();
  }
}

static void render(void) {
  if (run_on()) run_sample();
  render_board();
}

/* The run's frame loop: a frame whose answer differs from the last one's is drawn. */
static void run_frame(void) {
  if (!run_on()) return;
  AnimFrame was = FR;
  int landed = run_landed, opened = run_opened;
  run_sample();
  if (!run_on() || landed != run_landed || opened != run_opened || was.step != FR.step
      || was.in_flight_from_deck != FR.in_flight_from_deck || was.in_flight_to_flipped != FR.in_flight_to_flipped
      || !card_is_none(was.trump_flight) != !card_is_none(FR.trump_flight))
    render_board();
  if (run_on()) cd_raf_request(RAF_ANIM);
}

/* ---- moves ---------------------------------------------------------------- */

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
static double game_asked, game_answered;
static int game_adopted;

static uint32_t saves_asked, saves_answered; /* for the tests: a reload waits for the last put's answer */
static void game_save(void) {
  int len = game_keep_put(&G, game_blob);
  if (len > 0) { cd_store_put(STORE_GAME, GAME_KEY, sizeof GAME_KEY - 1, game_blob, (uint32_t)len); saves_asked++; }
}

/* A put that failed is not silent: a notice says the game is not saved (render_saved) until a later
 * put is kept, and the save is tried again SAVE_RETRY_MS later (one retry waiting at a time; every
 * move saves anyway). */
#define SAVE_RETRY_MS 2000
static int save_failed, save_retry_pending;
static cd_region save_notice;
static void render_saved(void) {
  if (!cd_region_show(&save_notice, save_failed)) return;
  cd_at(board, 0);
  OPEN("div");
  cd_name_open(save_notice.root);
  ATTR("data-save-failed", "true");
  STYLE("position: absolute; top: 8px; left: 50%; transform: translateX(-50%); font-size: 12px; "
        "color: var(--color-text-primary); text-shadow: var(--text-shadow-label); white-space: nowrap; "
        "z-index: 2001; pointer-events: none;");
  TEXT("Not saved: a reload would lose this game");
  CLOSE();
  cd_region_end(&save_notice);
}
void cd_on_store_put(uint32_t token, uint32_t ok) {
  if (token != STORE_GAME) return; /* the textures' puts: a cache */
  saves_answered++;
  save_failed = !ok;
  if (!ok && !save_retry_pending) { save_retry_pending = 1; cd_timer_start(SAVE_RETRY_MS, TIMER_SAVE_RETRY); }
  render_saved();
}

/* ---- the bot's pace (bot_drive.h bot_wait_ms) -------------------------------
 * The kernel says how long the bot waits: until the last operation shown has finished playing,
 * then BOT_PACE_MS_WITH_HUMANS (3000 ms) with a human in the game. The clock is the table's
 * BoardClock in this page's time: shown_ms is when the last operation (mine, the bot's, a deal,
 * a load) was shown, and settles_ms when it finishes playing: the run's last landing (the run is
 * the queue the operations' streams play in, so this is the server's BoardClock settles_ms, which
 * queues a stream behind the one in flight), or shown_ms with no run. A timer fires when
 * the wait is over and asks again, so a move of mine in between moves the bot's turn back, as
 * the server's loop does. A timer cannot be cancelled: each schedule has its own generation, and
 * a timer of an older one does nothing. */
static double shown_ms;
static uint32_t bot_gen; /* the generation of the live bot timer */
static int last_wait;    /* the kernel's last answer, for the test */
#ifdef TABLE_DEBUG
static int dbg_hold, dbg_due, dbg_fast; /* the test build: hold a due step; fast: 30 ms waits */
#endif

static double settles_ms(void) { return run_settles > shown_ms ? run_settles : shown_ms; }

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
  double settles = settles_ms();
#ifdef TABLE_DEBUG
  if (dbg_fast) settles -= BOT_PACE_MS_WITH_HUMANS - 30; /* fast: 30 ms after the run, as after the move */
#endif
  last_wait = bot_wait_ms(&G, game_human_mask(&G), (int64_t)shown_ms, (int64_t)settles, now);
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
/* My hand's order as drawn (the remembered order over the game's cards, new ones last) made the
 * game's own (game_rearrange_hand, the server's call for ASK_REARRANGE_HAND). After a drag or a
 * Meta+arrow, and after every operation: a card taken back keeps the slot it is drawn in, so a
 * reload, which draws the game's order, draws what was on screen. React's server holds the order
 * only a drag sent it, and a React reload puts such a card where the server has it. */
static void hand_into_game(void) {
  Player *p = &G.players[ME];
  Card order[MAX_HAND_SIZE];
  uint8_t idx[MAX_HAND_SIZE];
  client_hand_shown(hand_memory, hand_memory_n, p->hand, p->hand_count, order);
  for (int i = 0; i < p->hand_count; i++)
    for (int k = 0; k < p->hand_count; k++)
      if (card_eq(order[i], p->hand[k])) idx[i] = (uint8_t)k;
  game_rearrange_hand(&G, ME, idx, p->hand_count);
}

static void committed(void) {
  hand_into_game();
  game_keep_canonical(&G);
  game_save();
  mark_shown();
}

/* One bot step: whatever the bot does up to its next visible move. */
static uint32_t bot_steps; /* how many bot steps this load has run */
static void bot_step(void) {
  BotDriveOut out;
  bot_steps++;
  op_begin();
  bot_drive(&G, game_human_mask(&G), BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
  op_end(out.n ? out.actions[out.n - 1].seat : -1);
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
  op_begin();
  int ok = awire_apply(&G, ME, &a);
  op_end(ME); /* a refused move captured nothing: no steps */
  if (!ok) { last_reject = engine_last_reject; return 0; }
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

static int hand_index(int cid) {
  for (int i = 0; i < hand_n; i++) if (card_to_id(HAND[i]) == cid) return i;
  return -1;
}

/* The hand's order as drawn, kept in the game as a server keeps a seat's (game_rearrange_hand, the
 * move ASK_REARRANGE_HAND makes): a reload draws it so. React sends it 5 s after a drag; here the
 * game is local and it is kept at once. Not a move: the bot's clock is left alone. */
static void hand_keep(void) {
  hand_into_game();
  game_keep_canonical(&G);
  game_save();
}

/* ---- drag and drop (DragContext.tsx, DragShadow.tsx, TableBattles.tsx) ----------------------
 * Mouse and touch, as React: the press on a hand card, then the document's moves and release while
 * it lasts. A press released within 150 ms that neither moved 10 px nor rearranged is a tap, which
 * toggles the card; a press held longer does nothing. Moved 10 px it is a drag: within the hand's
 * area (the plane and one card's height above it) it swaps the card with the one under the
 * pointer, live; outside it, it is a game action, and the kernel says what the drop would be
 * (client_gesture_cards, client_drop_target, client_play), which the shadow names and the table
 * shows (the cover's cell in amber, the drop cells of an attack or a transfer). Let go, a move is
 * played as a pill plays it, and the selection is spent; anything else does nothing. */
enum { DRAG_SLOP_PX = 10, TAP_MS = 150 };
static const uint32_t DRAG_EVENTS[] = {CD_EV_MOUSEMOVE, CD_EV_TOUCHMOVE, CD_EV_MOUSEUP, CD_EV_TOUCHEND};
static void drag_listen(int on) {
  for (int k = 0; k < 4; k++) {
    if (on) cd_listen_window(DRAG_EVENTS[k]);
    else cd_unlisten_window(DRAG_EVENTS[k]);
  }
}

/* The kernel's reading of a drop where the pointer is now, on the board on screen (DragContext.tsx
 * determineGameAction), asked again only when the target or the cards changed (or DRAG.asked was
 * cleared: every render asks on the board it draws). 1 when the answer differs from the last one. */
static int drag_ask(void) {
  cd_id under = cd_hit(DRAG.x, DRAG.y);
  uint32_t tag = under ? cd_tag(under) : 0;
  int battle = -1;
  if (tag & TAG_BATTLE)
    for (int i = 0; i < V->num_battles; i++) if (card_to_id(V->battles[i].attack) == (int)(tag & 0xff)) battle = i;
  int8_t target = (int8_t)client_drop_target(V, 0, battle);
  Card picked[MAX_MOVE_CARDS], cards[MAX_MOVE_CARDS];
  for (int i = 0; i < nsel; i++) picked[i] = card_of_id(sel[i]);
  int n = client_gesture_cards(picked, nsel, card_of_id(DRAG.cid), cards);
  uint8_t ids[MAX_MOVE_CARDS];
  for (int i = 0; i < n; i++) ids[i] = (uint8_t)card_to_id(cards[i]);
  if (DRAG.asked && target == DRAG.asked_target && n == DRAG.asked_n
      && !__builtin_memcmp(ids, DRAG.asked_cards, (size_t)n)) return 0;
  ClientPlay was = DRAG.play;
  ClientGesture g;
  __builtin_memset(&g, 0, sizeof g);
  g.n_cards = (int8_t)n;
  g.target = target;
  for (int i = 0; i < n; i++) g.cards[i] = cards[i];
  const ClientPlayScratch sc = {&MOVES, MENU, (int)sizeof MENU};
  if (client_play(&CT, V, &g, &sc, &DRAG.play) != CLIENT_OK) DRAG.play.move_type = -1;
  DRAG.asked = 1;
  DRAG.asked_target = target;
  DRAG.asked_n = (int8_t)n;
  __builtin_memcpy(DRAG.asked_cards, ids, (size_t)n);
  if (was.move_type != DRAG.play.move_type || was.n_cards != DRAG.play.n_cards) return 1;
  for (int i = 0; i < DRAG.play.n_cards; i++)
    if (!card_eq(was.cards[i], DRAG.play.cards[i]) || !card_eq(was.attack_cards[i], DRAG.play.attack_cards[i])) return 1;
  return 0;
}

static void drag_start(const cd_event_rec *e, int cid) {
  int at = hand_index(cid);
  if (at < 0) return;
  if (e->type == CD_EV_MOUSEDOWN) cd_prevent_default(); /* no native drag of a draggable card, no text selection */
  if (DRAG.on) drag_listen(0);
  __builtin_memset(&DRAG, 0, sizeof DRAG);
  DRAG.on = 1;
  DRAG.index = at;
  DRAG.x0 = DRAG.x = e->x;
  DRAG.y0 = DRAG.y = e->y;
  DRAG.t0 = e->ms;
  drag_listen(1);
}

static void render_drag_position(void);
static void drag_move(const cd_event_rec *e) {
  if (!DRAG.on) return;
  if (e->type == CD_EV_TOUCHMOVE) cd_prevent_default(); /* the page does not scroll under a drag */
  int was_moving = DRAG.moving, was_action = DRAG.action, changed = 0;
  DRAG.x = e->x;
  DRAG.y = e->y;
  int dx = e->x - DRAG.x0, dy = e->y - DRAG.y0;
  if (dx * dx + dy * dy > DRAG_SLOP_PX * DRAG_SLOP_PX) DRAG.moving = 1;
  double r[4];
  cd_rect(hand_plane, r);
  int in_area = e->y >= r[1] - WEB_HAND.card_h;
  if (DRAG.moving && !in_area) {
    if (!DRAG.action) { DRAG.action = 1; DRAG.cid = card_to_id(HAND[DRAG.index]); DRAG.asked = 0; }
  } else if (DRAG.action && in_area) {
    DRAG.action = 0;
  }
  if (DRAG.moving && !DRAG.action) { /* a rearrange: the card under the pointer changes places with it */
    cd_id under = cd_hit(e->x, e->y);
    uint32_t tag = under ? cd_tag(under) : 0;
    int to = tag & TAG_CARD ? hand_index((int)(tag & 0xff)) : -1;
    if (to >= 0 && to != DRAG.index && client_hand_swap(hand_memory, &hand_memory_n, (int)sizeof hand_memory, HAND, hand_n, DRAG.index, to)) {
      DRAG.index = to;
      DRAG.swapped = 1;
      changed = 1;
    }
  }
  if (DRAG.action) changed |= drag_ask();
  if (changed || was_moving != DRAG.moving || was_action != DRAG.action) render_board();
  else render_drag_position();
}

static void drag_end(const cd_event_rec *e) {
  if (!DRAG.on) return;
  cd_prevent_default(); /* after a touch: no mouse events and no click */
  drag_listen(0);
  /* timed by the two events' own stamps: a release whose handler ran late is still a tap */
  int32_t held = e->ms - DRAG.t0;
  if (held >= 0 && held < TAP_MS && !DRAG.swapped && !DRAG.moving && DRAG.index < hand_n)
    toggle(card_to_id(HAND[DRAG.index]));
  if (DRAG.action && DRAG.moving) { /* the drop, where the pointer last moved */
    drag_ask();
    ClientPlay p = DRAG.play;
    if (p.move_type >= 0) nsel = 0;
    if (p.move_type == MOVE_ATTACK || p.move_type == MOVE_COVER || p.move_type == MOVE_PASS)
      play(p.move_type, p.cards, p.n_cards, p.move_type == MOVE_COVER ? p.attack_cards : 0);
  }
  if (DRAG.swapped) hand_keep();
  __builtin_memset(&DRAG, 0, sizeof DRAG);
}

/* The drag shadow (DragShadow.tsx, cards.css .drag-shadow): while a drag is a game action, over the
 * pointer 40 px up, a badge naming the drop the kernel reads (none for no move) over the cards the
 * drag carries. Rebuilt when the answer changes; moved with the pointer. */
static cd_region shadow;
static struct { int8_t move_type, n; uint8_t cards[MAX_MOVE_CARDS]; int x, y; } shadow_shown;
static void render_drag_position(void) {
  if (!shadow.shown || (shadow_shown.x == DRAG.x && shadow_shown.y == DRAG.y)) return;
  cd_set_style_num(shadow.root, ZS("left"), shadow_shown.x = DRAG.x, CD_UNIT_PX);
  cd_set_style_num(shadow.root, ZS("top"), (shadow_shown.y = DRAG.y) - 40, CD_UNIT_PX);
}
static void render_drag(void) {
  int want = DRAG.on && DRAG.action;
  int8_t mt = want ? DRAG.play.move_type : -1;
  if (want && shadow.shown && mt == shadow_shown.move_type && DRAG.asked_n == shadow_shown.n
      && !__builtin_memcmp(DRAG.asked_cards, shadow_shown.cards, (size_t)DRAG.asked_n)) {
    render_drag_position();
    return;
  }
  cd_region_hide(&shadow);
  if (!want || !cd_region_show(&shadow, 1)) return;
  shadow_shown.move_type = mt;
  shadow_shown.n = DRAG.asked_n;
  __builtin_memcpy(shadow_shown.cards, DRAG.asked_cards, (size_t)DRAG.asked_n);
  shadow_shown.x = DRAG.x;
  shadow_shown.y = DRAG.y;
  static SB s;
  sb_reset(&s);
  sb_px(&s, "left: ", DRAG.x);
  sb_px(&s, "top: ", DRAG.y - 40);
  cd_at(board, deck_area.parent); /* the board's first child after its title, as React mounts it */
  OPEN("div");
  cd_name_open(shadow.root);
  CLASS("drag-shadow");
  cd_attr(S_STYLE, sb_tmp(&s));
  if (mt == MOVE_ATTACK || mt == MOVE_COVER || mt == MOVE_PASS) {
    OPEN("div");
    CLASS("drag-shadow__indicator");
    if (mt == MOVE_PASS) { TEXT("\xF0\x9F\x94\x84"); TEXT(" "); TEXT("Pass"); } /* the theme's pass icon, then t('pass') */
    else {
      OPEN("span"); CLASS("");
      if (mt == MOVE_ATTACK) TEXT("\xE2\x9A\x94\xEF\xB8\x8F"); else TEXT("\xF0\x9F\x9B\xA1\xEF\xB8\x8F");
      CLOSE();
      TEXT(" ");
      if (mt == MOVE_ATTACK) TEXT("Attack"); else TEXT("Cover");
    }
    CLOSE();
  }
  OPEN("div");
  CLASS("drag-shadow__cards");
  for (int i = 0; i < DRAG.asked_n; i++) {
    Card c = card_of_id(DRAG.asked_cards[i]);
    face_open(c, 0);
    sb_reset(&s);
    sb_s(&s, "background-color: var(--color-card-face); width: 50px; height: 70px; border-radius: 5px; border: 2px solid black; "
             "box-shadow: rgba(0, 0, 0, 0.3) 0px 2px 4px; cursor: default; user-select: none; -webkit-user-select: none; "
             "pointer-events: none; transition: transform 0.2s ease-in-out, opacity 0.2s ease-in-out, box-shadow 0.2s ease-in-out; "
             "position: relative; font-family: Georgia, serif; font-weight: bold; color: ");
    sb_s(&s, card_red(c) ? "rgb(220, 38, 38); " : "rgb(0, 0, 0); ");
    sb_s(&s, "display: flex; flex-direction: column; font-size: 20px; line-height: 15px; transform: scale(0.8); "
             "transform-origin: center center; margin-right: 2px; opacity: 0.9;");
    cd_attr(S_STYLE, sb_tmp(&s));
    CLOSE();
  }
  CLOSE();
  CLOSE();
  cd_region_end(&shadow);
}

/* ---- keyboard play (KeyboardInputHandler.tsx, KeyboardPlayMode.tsx) -------------------------
 * Two systems on the one keydown, as React mounts both on the live table. Letters and numbers:
 * 1-9, 0, - and = toggle the hand's cards 1 to 12; Space or A attacks with the selection and Space
 * or C covers with it (the Cover button's aim), by whether I defend; G says Good, P passes, U
 * takes; each only when its pill shows, and none with Ctrl, Alt or Meta held. Arrows: a cursor on
 * the hand; Shift toggles the card under it, Shift+Left/Right sweeps and toggles, Meta+Left/Right
 * moves the card; Up plays (the selection as the buttons would, or the cursor's card as the kernel
 * reads it, client_cursor_play: a card that could cover more than one battle, or cover and
 * transfer, aims first, Left/Right cycling the targets and Up playing the one shown, Down giving
 * up), Down takes or says Good. No key does anything typed into a field. */
static void kb_after_move(void) { KB.n_targets = 0; } /* the aim goes; the cursor stays */

/* A key's move, as KeyboardPlayMode fires it: its pill pressed until the move is refused, the
 * selection spent when it was the selection's and the move was taken. */
static void kb_fire(uint32_t bit, int kind, const Card *cards, int n, const Card *attacks, int spend) {
  pressed |= bit;
  int ok = play(kind, cards, n, attacks);
  if (ok && spend) nsel = 0;
  if (!ok) pressed &= ~bit;
  kb_after_move();
}

static void kb_take_or_good(void) {
  uint32_t pills = pills_for(1);
  if (pills & PLAY_PILL_PICKUP) kb_fire(PLAY_PILL_PICKUP, AWIRE_PICKUP, 0, 0, 0, 0);
  else if (pills & PLAY_PILL_GOOD) kb_fire(PLAY_PILL_GOOD, AWIRE_GOOD, 0, 0, 0, 0);
}

enum { KEY_SHIFT = 16, KEY_LEFT = 37, KEY_UP = 38, KEY_RIGHT = 39, KEY_DOWN = 40 };

/* A letter's move: its pill pressed until the move is refused. */
static void letter_fire(uint32_t bit, int kind, const Card *cards, int n, const Card *attacks) {
  pressed |= bit;
  if (!play(kind, cards, n, attacks)) pressed &= ~bit;
}

/* KeyboardInputHandler: the letters and numbers. */
static void kb_letters(const cd_event_rec *e) {
  if (e->mods & (CD_MOD_CTRL | CD_MOD_ALT | CD_MOD_META)) return;
  int ch = e->ch >= 'A' && e->ch <= 'Z' ? e->ch + 32 : e->ch;
  int pos = ch >= '1' && ch <= '9' ? ch - '0' : ch == '0' ? 10 : ch == '-' ? 11 : ch == '=' ? 12 : 0;
  if (pos) {
    cd_prevent_default();
    if (pos <= hand_n) toggle(card_to_id(HAND[pos - 1]));
    return;
  }
  int defending = V->defender == V->my_seat;
  uint32_t pills = pills_for(1);
  Card cards[MAX_MOVE_CARDS];
  int n = nsel;
  for (int i = 0; i < n; i++) cards[i] = card_of_id(sel[i]);
  /* A letter's move presses its pill as the pill's click and the arrows' Up do (React's letters do
   * not, and their pill stays up until the server's board comes: here that was the run's 500 ms,
   * the Good pill standing beside the check it had already turned). */
  if ((ch == ' ' || ch == 'a') && !defending) {
    cd_prevent_default();
    if (pills & PLAY_PILL_ATTACK) { nsel = 0; letter_fire(PLAY_PILL_ATTACK, AWIRE_ATTACK, cards, n, 0); }
  } else if ((ch == ' ' || ch == 'c') && defending) {
    cd_prevent_default();
    ClientPlay p;
    if (nsel && ask_play(CLIENT_PLAY_COVER_BUTTON, &p) == CLIENT_OK && p.move_type == MOVE_COVER) {
      nsel = 0;
      letter_fire(PLAY_PILL_COVER, AWIRE_COVER, p.cards, p.n_cards, p.attack_cards);
    }
  } else if (ch == 'g') {
    cd_prevent_default();
    if (pills & PLAY_PILL_GOOD) letter_fire(PLAY_PILL_GOOD, AWIRE_GOOD, 0, 0, 0);
  } else if (ch == 'p') {
    cd_prevent_default();
    if (pills & PLAY_PILL_PASS) { nsel = 0; letter_fire(PLAY_PILL_PASS, AWIRE_PASS, cards, n, 0); }
  } else if (ch == 'u') {
    cd_prevent_default();
    if (pills & PLAY_PILL_PICKUP) letter_fire(PLAY_PILL_PICKUP, AWIRE_PICKUP, 0, 0, 0);
  }
}

/* KeyboardPlayMode: Shift and the arrows. */
static void kb_arrows(const cd_event_rec *e) {
  int shift = e->mods & CD_MOD_SHIFT, other = e->mods & (CD_MOD_CTRL | CD_MOD_ALT | CD_MOD_META);
  int k = e->key, sideways = k == KEY_LEFT || k == KEY_RIGHT;
  if (k == KEY_SHIFT) {
    if (other || KB.sel < 0 || KB.sel >= hand_n || KB.n_targets) return;
    cd_prevent_default();
    toggle(card_to_id(HAND[KB.sel]));
    return;
  }
  if (k < KEY_LEFT || k > KEY_DOWN) return;
  if (shift && !other && sideways) { /* sweep-select */
    cd_prevent_default();
    if (!hand_n || KB.n_targets) return;
    KB.sel = KB.sel < 0 ? 0 : k == KEY_LEFT ? (KB.sel - 1 + hand_n) % hand_n : (KB.sel + 1) % hand_n;
    toggle(card_to_id(HAND[KB.sel]));
    return;
  }
  if ((e->mods & CD_MOD_META) && !(e->mods & (CD_MOD_CTRL | CD_MOD_ALT)) && sideways) { /* rearrange */
    cd_prevent_default();
    if (KB.sel < 0 || KB.sel >= hand_n) return;
    int to = k == KEY_LEFT ? KB.sel - 1 : KB.sel + 1;
    if (to < 0 || to >= hand_n) return;
    if (client_hand_swap(hand_memory, &hand_memory_n, (int)sizeof hand_memory, HAND, hand_n, KB.sel, to)) { KB.sel = to; hand_keep(); }
    return;
  }
  if (shift || other) return; /* a held modifier never plays */
  cd_prevent_default();
  if (KB.n_targets) { /* aiming */
    if (k == KEY_LEFT) KB.idx = (KB.idx - 1 + KB.n_targets) % KB.n_targets;
    else if (k == KEY_RIGHT) KB.idx = (KB.idx + 1) % KB.n_targets;
    else if (k == KEY_DOWN) KB.n_targets = 0;
    else if (KB.sel < 0 || KB.sel >= hand_n) KB.n_targets = 0;
    else {
      Card c = HAND[KB.sel];
      if (KB.battle[KB.idx] < 0) kb_fire(PLAY_PILL_PASS, AWIRE_PASS, &c, 1, 0, 0);
      else kb_fire(PLAY_PILL_COVER, AWIRE_COVER, &c, 1, &KB.attack[KB.idx], 0);
    }
    return;
  }
  if (KB.sel < 0) {
    if (k == KEY_DOWN) kb_take_or_good();
    else if (sideways && hand_n) KB.sel = 0;
    return;
  }
  if (!hand_n) { KB.sel = -1; return; }
  if (k == KEY_LEFT) { KB.sel = (KB.sel - 1 + hand_n) % hand_n; return; }
  if (k == KEY_RIGHT) { KB.sel = (KB.sel + 1) % hand_n; return; }
  if (KB.sel >= hand_n) return;
  if (k == KEY_DOWN) { kb_take_or_good(); return; }
  int defending = V->defender == V->my_seat;
  if (nsel) { /* Up commits the selection, as the buttons would */
    Card cards[MAX_MOVE_CARDS];
    int n = nsel;
    for (int i = 0; i < n; i++) cards[i] = card_of_id(sel[i]);
    uint32_t pills = pills_for(1);
    if (!defending) { if (pills & PLAY_PILL_ATTACK) kb_fire(PLAY_PILL_ATTACK, AWIRE_ATTACK, cards, n, 0, 1); return; }
    ClientPlay p;
    if (ask_play(CLIENT_PLAY_COVER_BUTTON, &p) == CLIENT_OK && p.move_type == MOVE_COVER)
      kb_fire(PLAY_PILL_COVER, AWIRE_COVER, p.cards, p.n_cards, p.attack_cards, 1);
    else if (pills & PLAY_PILL_PASS) kb_fire(PLAY_PILL_PASS, AWIRE_PASS, cards, n, 0, 1);
    return;
  }
  Card c = HAND[KB.sel];
  ClientPlay p;
  const ClientPlayScratch sc = {&MOVES, MENU, (int)sizeof MENU};
  int kind = client_cursor_play(&CT, V, c, &sc, &p);
  if (kind == CLIENT_CURSOR_ATTACK) kb_fire(PLAY_PILL_ATTACK, AWIRE_ATTACK, &c, 1, 0, 0);
  else if (kind == CLIENT_CURSOR_PASS) kb_fire(PLAY_PILL_PASS, AWIRE_PASS, &c, 1, 0, 0);
  else if (kind == CLIENT_CURSOR_COVER) kb_fire(PLAY_PILL_COVER, AWIRE_COVER, &c, 1, &V->battles[p.coverable[0]].attack, 0);
  else if (kind == CLIENT_CURSOR_CHOOSE) {
    KB.n_targets = 0;
    for (int i = 0; i < p.n_coverable; i++) {
      KB.battle[KB.n_targets] = p.coverable[i];
      KB.attack[KB.n_targets++] = V->battles[p.coverable[i]].attack;
    }
    if (p.pills & PLAY_PILL_PASS) KB.battle[KB.n_targets++] = -1;
    KB.idx = 0;
  }
}

/* The cursor's and the arrow's boxes polled every frame after a draw, as React's are, until they
 * have stood still KB_STILL_FRAMES frames (a slide or a glide may start 25 ms late). */
enum { RAF_KBD = 5, KB_STILL_FRAMES = 4 };
static int kb_polling, kb_still, kb_moved; /* kb_moved: the last render_keys moved the cursor or the arrow */
static void render_keys(void);
static void kb_poll_start(void) {
  kb_still = 0;
  if (!kb_polling && KB.sel >= 0) { kb_polling = 1; cd_raf_request(RAF_KBD); }
}
static void kb_poll_frame(void) {
  kb_polling = 0;
  kb_moved = 0;
  render_keys();
  kb_still = kb_moved ? 0 : kb_still + 1;
  if (KB.sel >= 0 && kb_still < KB_STILL_FRAMES) { kb_polling = 1; cd_raf_request(RAF_KBD); }
}

static void on_key(const cd_event_rec *e) {
  if (e->mods & CD_MOD_EDITING) return;
  kb_letters(e);
  kb_arrows(e);
}

/* What the keyboard shows (KeyboardPlayMode.tsx): the hidden state the tests read, the cursor (a
 * short dark underline below the card, from the kernel's slot for it, not a border: the red border
 * is the selection's), and while aiming the red arrow from the card's top to the target: a cover's
 * attack card boxed, or the transfer's dashed place right of the row with PASS under it. */
static cd_id kbd_state;
static cd_region kb_cursor, kb_arrow;
static struct { int8_t mode; int sel_cid, target; double cur[4], arrow[6]; } kb_shown = {-1, -2, -2, {0}, {0}};
static void kb_attr_card(cd_id id, cd_str name, int cid) {
  if (cid < 0) { cd_set_attr(id, name, ZS("")); return; }
  Card c = card_of_id((uint8_t)cid);
  char b[8] = {(char)('0' + c.suit), '-'};
  uint32_t n = 2 + cd_fmt(b + 2, c.value, 0, CD_UNIT_NONE);
  cd_set_attr(id, name, cd_tmp(b, n));
}
static void svg_num(const char *name, double v) {
  char b[32];
  cd_attr(cd_internz(name), cd_tmp(b, cd_fmt_len(b, v, CD_UNIT_NONE)));
}
static void render_keys(void) {
  if (KB.sel >= hand_n) KB.sel = hand_n ? hand_n - 1 : -1; /* the cursor stays where it was, or on the last card */
  int mode = KB.n_targets ? 2 : KB.sel >= 0 ? 1 : 0;
  int sel_cid = KB.sel >= 0 ? card_to_id(HAND[KB.sel]) : -1;
  int target = !KB.n_targets ? -2 : KB.battle[KB.idx] < 0 ? -1 : card_to_id(KB.attack[KB.idx]);
  if (mode != kb_shown.mode) {
    static const char *const MODE[] = {"none", "cards", "target"};
    cd_set_attr(kbd_state, ZS("data-mode"), cd_internz(MODE[kb_shown.mode = (int8_t)mode]));
  }
  if (sel_cid != kb_shown.sel_cid) kb_attr_card(kbd_state, ZS("data-sel"), kb_shown.sel_cid = sel_cid);
  if (target != kb_shown.target) {
    kb_shown.target = target;
    if (target == -1) cd_set_attr(kbd_state, ZS("data-target"), ZS("pass"));
    else kb_attr_card(kbd_state, ZS("data-target"), target < 0 ? -1 : target);
  }
  /* the cursor's card where it is drawn now: React polls the card's box every frame, so the cursor
   * and the arrow follow the hand's 0.1 s slide and the row's glide; kb_poll does the same while
   * they move (before the card is drawn, the kernel's slot for it on the plane) */
  double cur[4] = {0}, pr[4];
  if (sel_cid >= 0 && shown[sel_cid].node) cd_rect(shown[sel_cid].node, cur);
  else if (sel_cid >= 0 && KB.sel < HL.n) {
    cd_rect(hand_plane, pr);
    const HandRect *slot = &HL.slot[KB.sel];
    cur[0] = pr[0] + slot->x; cur[1] = pr[1] + slot->y; cur[2] = slot->w; cur[3] = slot->h;
  }
  /* the arrow's end: the attack card's box, or the transfer's point */
  double to[4] = {0};
  int pass = target == -1, aim = mode == 2 && sel_cid >= 0;
  if (aim && !pass && battle_attack[target]) cd_rect(battle_attack[target], to);
  else if (aim && pass) {
    double right = -1e9, top = 1e9, bottom = -1e9, r[4];
    int n = 0;
    for (uint32_t i = 0; i < n_row_keys; i++) {
      int aid = (int)row_keys[i] - 1;
      if (!battle_cell[aid]) continue;
      cd_rect(battle_cell[aid], r);
      if (r[0] + r[2] > right) right = r[0] + r[2];
      if (r[1] < top) top = r[1];
      if (r[1] + r[3] > bottom) bottom = r[1] + r[3];
      n++;
    }
    cd_viewport_rec vp;
    cd_viewport(&vp);
    to[0] = n ? right + 46 : vp.w / 2.0;
    to[1] = n ? (top + bottom) / 2 : vp.h / 2.0;
  } else aim = 0;
  if (!(cur[2] > 0)) cd_region_hide(&kb_cursor);
  else if (cd_region_show(&kb_cursor, 1)) {
    cd_at(container, kb_arrow.shown ? kb_arrow.root : board);
    OPEN("div");
    cd_name_open(kb_cursor.root);
    STYLE("position: fixed; left: 0px; top: 0px; width: 0px; height: 3px; background: rgb(17, 17, 17); border-radius: 2px; "
          "box-shadow: rgba(255, 255, 255, 0.7) 0px 0px 5px 1px; pointer-events: none; z-index: 1500;");
    CLOSE();
    cd_region_end(&kb_cursor);
    kb_shown.cur[2] = -1;
  }
  if (kb_cursor.shown && __builtin_memcmp(cur, kb_shown.cur, sizeof cur)) {
    kb_moved = 1;
    __builtin_memcpy(kb_shown.cur, cur, sizeof cur);
    cd_set_style_num(kb_cursor.root, ZS("left"), cur[0] + cur[2] * 0.15, CD_UNIT_PX);
    cd_set_style_num(kb_cursor.root, ZS("top"), cur[1] + cur[3] + 4, CD_UNIT_PX);
    cd_set_style_num(kb_cursor.root, ZS("width"), cur[2] * 0.7, CD_UNIT_PX);
  }
  double arrow[6] = {cur[0] + cur[2] / 2, cur[1], pass ? to[0] : to[0] + to[2] / 2, pass ? to[1] : to[1] + to[3] / 2, pass, target};
  if (!aim || !(cur[2] > 0)) { cd_region_hide(&kb_arrow); return; }
  if (kb_arrow.shown && !__builtin_memcmp(arrow, kb_shown.arrow, sizeof arrow)) return;
  kb_moved = 1;
  __builtin_memcpy(kb_shown.arrow, arrow, sizeof arrow);
  cd_region_hide(&kb_arrow);
  cd_region_show(&kb_arrow, 1);
  cd_at(container, board);
  OPEN("svg");
  cd_name_open(kb_arrow.root);
  STYLE("position: fixed; inset: 0px; width: 100%; height: 100%; pointer-events: none; z-index: 1500;");
  OPEN("defs");
  OPEN("marker"); ATTR("id", "kbd-arrow"); ATTR("markerWidth", "11"); ATTR("markerHeight", "8"); ATTR("refX", "9"); ATTR("refY", "4"); ATTR("orient", "auto");
  OPEN("polygon"); ATTR("points", "0 0, 11 4, 0 8"); ATTR("fill", "#ff2d2d"); CLOSE();
  CLOSE();
  CLOSE();
  OPEN("line");
  svg_num("x1", arrow[0]); svg_num("y1", arrow[1]); svg_num("x2", arrow[2]); svg_num("y2", arrow[3]);
  ATTR("stroke", "#ff2d2d"); ATTR("stroke-width", "3");
  if (pass) ATTR("stroke-dasharray", "7 5");
  ATTR("marker-end", "url(#kbd-arrow)");
  CLOSE();
  if (!pass) {
    OPEN("rect");
    svg_num("x", to[0] - 3); svg_num("y", to[1] - 3); svg_num("width", to[2] + 6); svg_num("height", to[3] + 6);
    ATTR("rx", "8"); ATTR("fill", "none"); ATTR("stroke", "#ff2d2d"); ATTR("stroke-width", "3");
    CLOSE();
  } else {
    OPEN("g");
    OPEN("rect");
    svg_num("x", to[0] - 26); svg_num("y", to[1] - 36); ATTR("width", "52"); ATTR("height", "72");
    ATTR("rx", "8"); ATTR("fill", "none"); ATTR("stroke", "#ff2d2d"); ATTR("stroke-width", "3"); ATTR("stroke-dasharray", "6 4");
    CLOSE();
    OPEN("text");
    svg_num("x", to[0]); svg_num("y", to[1] + 54); ATTR("fill", "#ff2d2d"); ATTR("font-size", "13"); ATTR("font-weight", "700");
    ATTR("text-anchor", "middle"); TEXT("PASS");
    CLOSE();
    CLOSE();
  }
  CLOSE();
  cd_region_end(&kb_arrow);
}

/* A game on the table that was not played to it here (a deal, a load): nothing of the last one's
 * selection or presses carries over. */
static void drag_listen(int on);
static void table_reset(void) {
  nsel = 0;
  pressed = 0;
  prev_rearm = 0;
  hand_memory_n = 0; /* the order starts as the game's */
  if (DRAG.on) drag_listen(0);
  __builtin_memset(&DRAG, 0, sizeof DRAG);
  KB.sel = -1;
  KB.n_targets = 0;
}

static void new_game(void) {
  table_reset();
  if (run_on()) run_end();
  op_begin();
  deal_new();
  op_end(-1);
  committed();
  bot_check();
  render();
}

/* The store's answer to the start's ask, the table's first render: the game left on an earlier
 * load when it reads as one this table plays, else the seeded deal. Until it answers (at most
 * CD_STORE_DEADLINE_MS) the page shows its frame and no board: measured (README, Reload), drawing
 * the seeded deal at once and the kept game over it moved first contentful paint from 70 to 136 ms
 * at x1 and from 195 to 340 at x4, and holding the board does not move it (the store answers at 54
 * ms, before the stylesheet's first paint at 70). */
static void game_stored(const uint8_t *p, int32_t len) {
  game_answered = cd_now();
  game_adopted = len > 0 && game_keep_load(&G, p, len);
  table_reset();
  game_keep_canonical(&G);
  mark_shown();
  bot_check();
  render();
}

/* A click: Play again, a pill, or the selection put down on a battle or the table. */
static void on_click(uint32_t tag) {
  if (tag & TAG_PILL) on_pill(tag & 0xff);
  else if (tag & TAG_BATTLE) {
    int aid = (int)(tag & 0xff), b = -1;
    for (int i = 0; i < V->num_battles; i++) if (card_to_id(V->battles[i].attack) == aid) b = i;
    if (b >= 0) drop_selection((int8_t)b);
  } else if (tag & TAG_TABLE) drop_selection(PLAY_TARGET_TABLE);
}

void cd_on_event(const cd_event_rec *e) {
  if (!V) return; /* no board yet: the store has not answered */
  if (e->type == CD_EV_RESIZE) { render(); return; } /* the hand is laid out for the new width */
  uint32_t tag = cd_tag((cd_id)e->id);
  switch (e->type) {
  case CD_EV_MOUSEDOWN: case CD_EV_TOUCHSTART: /* a hand card pressed */
    if ((tag & TAG_CARD) && V->status == GAME_STATUS_PLAYING) drag_start(e, (int)(tag & 0xff));
    return;
  case CD_EV_MOUSEMOVE: case CD_EV_TOUCHMOVE: drag_move(e); return; /* it draws what changed */
  case CD_EV_MOUSEUP: case CD_EV_TOUCHEND: drag_end(e); break;
  case CD_EV_KEYDOWN: on_key(e); break;
  case CD_EV_CLICK:
    if (tag & TAG_AGAIN) { new_game(); return; }
    if (V->status != GAME_STATUS_PLAYING) return;
    on_click(tag);
    break;
  default: return;
  }
  bot_check();
  render();
}

void cd_on_timer(uint32_t token) {
  if (token & TIMER_ARM) { flights_arm(token & ~TIMER_ARM); return; }
  if (token >= TIMER_BOT_BASE) bot_due(token - TIMER_BOT_BASE);
#ifdef TABLE_DEBUG
  if (token == TIMER_BOT_GO) { bot_pending = 1; bot_step(); }
#endif
  if (token == TIMER_TEX) tex_go();
  if (token == TIMER_SAVE_RETRY) { save_retry_pending = 0; game_save(); }
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
/* The animations, for the tests: 1 while a run plays or a mark is turning; how many runs have started and how many
 * flights were made on this load; the run's step count, and step k's type, start and duration (the
 * kernel's plan). */
EXPORT("app_anim_busy") int32_t app_anim_busy(void) { return run_on() || coins_turning; }
EXPORT("app_anim_handoffs") uint32_t app_anim_handoffs(void) { return handoffs; } /* role marks thrown seat to seat */
EXPORT("app_anim_runs") uint32_t app_anim_runs(void) { return runs_started; }
EXPORT("app_anim_flights") int32_t app_anim_flights(void) { return flights_made; }
EXPORT("app_anim_steps") int32_t app_anim_steps(void) { return run_n; }
EXPORT("app_anim_step") int32_t app_anim_step(int32_t k, int32_t f) {
  if (k < 0 || k >= run_n) return -1;
  return f == 0 ? run[k].ev.type : f == 1 ? PLAN.steps[k].start_ms : f == 2 ? PLAN.steps[k].duration_ms : f == 3 ? run[k].ev.seat : run[k].ev.n_cards;
}
/* When the run started (cd_now ms), and where it stands: the steps landed and opened. */
EXPORT("app_anim_origin") double app_anim_origin(void) { return run_origin; }
EXPORT("app_anim_landed") int32_t app_anim_landed(void) { return run_landed; }
/* The cards in the air now: how many flights, and flight i's card id (-1: a stack of backs). */
EXPORT("app_anim_flying") int32_t app_anim_flying(void) { return n_flights; }
EXPORT("app_anim_flight_card") int32_t app_anim_flight_card(int32_t i) { return i >= 0 && i < n_flights ? flight[i].cid : -2; }
EXPORT("app_saves_pending") uint32_t app_saves_pending(void) { return saves_asked - saves_answered; }
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
/* The run's clock this many times faster (the games the tests play out): the kernel's plan is not
 * touched, only how fast its time passes here. */
EXPORT("app_debug_anim_speed") void app_debug_anim_speed(double k) { anim_speed = k > 0 ? k : 1; }
/* The golden's stock with my Qd attacking and the bot holding Qh: it transfers (the handwritten
 * brain always does here), for the check that the shield moves as the card leaves. */
EXPORT("app_debug_transfer") void app_debug_transfer(void) {
  table_reset();
  if (run_on()) run_end();
  compose("Qd", "6s 7h Ad 6d", "Qh 8h 9h 6c");
  committed();
  bot_pending = 0;
  bot_gen++;
  bot_check();
  render();
}
/* A new deal now, as Play again deals one (the deal's run, one card at a time). */
EXPORT("app_debug_deal") void app_debug_deal(void) { new_game(); }
/* Boards on the golden's stock for the drag and key checks (trump clubs, the Kc turned; every card
 * once, outside the golden's stock):
 *   1  I defend 7h and Qd holding 9h Kd 6c 6s: 9h covers only 7h, Kd only Qd, the trump 6c either
 *      (two targets), 6s nothing; and Take is offered;
 *   2  I defend 6h holding 6c 6s 9h Kd: 6s only transfers, 6c covers or transfers, 9h only covers,
 *      Kd neither;
 *   3  I lead over 6h/8h and 6d/Kd, all covered, holding 6s 7h Qd Ad: Good is offered (0 is the same);
 *   4  the golden's board with the seats swapped: I defend 6h/8h and 6d holding 9h Th Jh 6c. */
static void debug_compose(int32_t k) {
  if (k == 1) compose("7h Qd", "9h Kd 6c 6s", "Jh Qh Kh Ah");
  else if (k == 2) compose("6h", "6c 6s 9h Kd", "Jh Qh Kh Ah");
  else if (k == 4) compose("6h/8h 6d", "9h Th Jh 6c", "6s 7h Qd Ad");
  else compose("6h/8h 6d/Kd", "6s 7h Qd Ad", "9h Th Jh 6c");
  if (k == 1 || k == 2 || k == 4) { G.first_attacker = 1; G.defender = 0; }
}
EXPORT("app_debug_board") void app_debug_board(int32_t k) {
  table_reset();
  if (run_on()) run_end();
  debug_compose(k);
  committed();
  bot_pending = 0;
  bot_gen++;
  bot_check();
  render();
}
/* The keyboard's cursor and aim, for the tests: the cursor's index (-1 none), the targets' count. */
EXPORT("app_kb") int32_t app_kb(int32_t which) { return which == 0 ? KB.sel : KB.n_targets; }
/* The drag, for the tests: 0 none, 1 pressed, 2 a rearrange, 3 a game action; and the move the
 * drop would make (MOVE_*, -1 none). */
EXPORT("app_drag") int32_t app_drag(int32_t which) {
  if (which == 1) return DRAG.on && DRAG.action ? DRAG.play.move_type : -1;
  return !DRAG.on ? 0 : !DRAG.moving ? 1 : DRAG.action ? 3 : 2;
}
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
 * Play again 149 x 48, the results panel 352 by 81 for two rows, ReplayShare's frame 188 x 188). The wood is made
 * only there, so the less, the sooner it lands. tools/test.mjs checks, after every click of
 * played-out games, that each surface on the page lies inside the rectangle made for it. */
static const struct { double seed; int32_t w, h; } WOOD_SURF[] = {{SEED_BACK, 56, 56}, {SEED_AGAIN, 200, 60}, {SEED_RESULTS, 360, 100}, {SEED_REPLAY, 188, 188}};
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

/* A texture's image file lands: its url goes on the mount once the browser has decoded it
 * (cd_on_image), so no frame shows the property before its picture. */
static void tex_land(int k, const uint8_t *p, uint32_t len) {
  cd_set_image_bytes(CD_MOUNT, cd_internz(TEX_PROP[k]), p, len, cd_internz(TEX_MIME[k]), (uint32_t)k);
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

/* The picture is on: with the fern, the backs turn black under it, as CardBack's do, in the same
 * frame as the fern (set earlier, every back showed a frame of black before its fern). A kept
 * picture that no longer decodes is made again. */
void cd_on_image(uint32_t token, uint32_t ok) {
  int k = (int)token;
  if (k < 0 || k >= NTEX) return;
  if (!ok && tex.from_store[k]) { tex.from_store[k] = 0; tex_state[k] = TEX_MISSING; tex_make_next(); return; }
  if (ok && k == TEX_FERN) cd_set_style(CD_MOUNT, ZS("--tex-fern-bg"), ZS("#000000"));
  tex.at[k] = cd_now();
  tex.landed++;
  tex_make_next();
}

void cd_on_raf(uint32_t token, double t) {
  (void)t;
  if (token == RAF_ANIM) { run_frame(); return; }
  if (token == RAF_COIN) { coins_frame(); return; }
  if (token == RAF_KBD) { kb_poll_frame(); return; }
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
  cd_name_open(kbd_state = cd_id_new());
  ATTR("data-testid", "kbd-state"); ATTR("data-mode", "none"); ATTR("data-sel", ""); ATTR("data-target", ""); STYLE("display: none;");
  CLOSE();

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

  cd_listen_window(CD_EV_RESIZE);
  cd_listen_window(CD_EV_KEYDOWN);
  game_asked = cd_now();
  cd_store_get(STORE_GAME, GAME_KEY, sizeof GAME_KEY - 1, CD_STORE_DEADLINE_MS);
  /* the board is drawn when the store answers (game_stored) */
  if (!page_notex()) {
    textures_begin();                /* the store is asked now; what it has lands when read */
    cd_raf_request(RAF_PAINTED);     /* what it has not is made once the table has painted */
  }
}

