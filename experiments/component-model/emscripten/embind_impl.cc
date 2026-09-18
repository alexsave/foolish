// The third variant: the same KGame round trip, expressed with Emscripten's
// embind. Built by build.sh when emcc is on PATH, skipped with a note when it
// is not. See docs/CODEGEN_ALTERNATIVES.md.
//
// embind is given the fairest shape available to it: `emscripten::val`, which
// takes and returns PLAIN JavaScript objects and arrays, so all three variants
// start from and end at the same JS value the bench builds. The alternative,
// `value_object` plus `register_vector`, would make JS construct wrapper
// objects (`new Module.VectorUint8()`) and would not be the same comparison.
//
// Note what this file cannot avoid: it is C++, not C, and it links a libc, a
// C++ runtime with exceptions and RTTI, and an allocator - the three things the
// repo's kernel deliberately does not have.
#include <emscripten/bind.h>
#include <emscripten/val.h>

extern "C" {
#include "../c/kstate.h"
}

KGame g_game;

using emscripten::val;

static int clamp_i(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

static const char *const G_STATUS[] = {"waiting", "playing", "finished"};
static const char *const P_STATUS[] = {"idle", "ready", "playing", "out"};

static int status_index(const val &v, const char *const *names, int n) {
    const std::string s = v.as<std::string>();
    for (int i = 0; i < n; i++)
        if (s == names[i]) return i;
    return 0;
}

static void import_state(val g) {
    KGame *k = &g_game;
    k->status = (int8_t)status_index(g["status"], G_STATUS, 3);
    const val players = g["players"];
    k->num_players = (int8_t)clamp_i(players["length"].as<int>(), MAX_PLAYERS);
    k->power_suit = (int8_t)g["trumpSuit"].as<int>();
    k->first_attacker = (int8_t)g["firstAttacker"].as<int>();
    k->defender = (int8_t)g["defender"].as<int>();
    k->discard = (int16_t)g["discard"].as<int>();
    const val flipped = g["flipped"];
    k->has_flipped = !flipped.isUndefined() && !flipped.isNull();
    k->flipped = (uint8_t)(k->has_flipped ? flipped.as<int>() : 0);
    k->good_mask = (uint32_t)g["goodMask"].as<unsigned>();
    k->has_good_ts = g["hasGoodTs"].as<bool>();

    const val deck = g["deck"];
    k->deck_count = (int16_t)clamp_i(deck["length"].as<int>(), MAX_DECK);
    for (int i = 0; i < k->deck_count; i++) k->deck[i] = (uint8_t)deck[i].as<int>();

    const val battles = g["battles"];
    k->num_battles = (int8_t)clamp_i(battles["length"].as<int>(), MAX_BATTLES);
    for (int i = 0; i < k->num_battles; i++) {
        const val b = battles[i];
        k->battles[i].attack = (uint8_t)b["attack"].as<int>();
        const val d = b["defense"];
        k->battles[i].defense = (d.isUndefined() || d.isNull()) ? WIRE_NONE : (uint8_t)d.as<int>();
    }

    for (int i = 0; i < k->num_players; i++) {
        KPlayer *p = &k->players[i];
        const val wp = players[i];
        p->status = (int8_t)status_index(wp["status"], P_STATUS, 4);
        p->awaiting = wp["awaiting"].as<bool>();
        const val hand = wp["hand"];
        p->hand_count = (int8_t)clamp_i(hand["length"].as<int>(), MAX_HAND_SIZE);
        for (int j = 0; j < p->hand_count; j++) p->hand[j] = (uint8_t)hand[j].as<int>();
    }

    const val elim = g["elimination"];
    k->num_eliminated = (int8_t)clamp_i(elim["length"].as<int>(), MAX_PLAYERS);
    for (int i = 0; i < k->num_eliminated; i++) k->elimination[i] = (int8_t)elim[i].as<int>();
}

static val export_state() {
    const KGame *k = &g_game;
    val g = val::object();
    g.set("status", val(std::string(G_STATUS[k->status < 0 || k->status > 2 ? 0 : k->status])));
    g.set("numPlayers", val((int)k->num_players));
    g.set("trumpSuit", val((int)k->power_suit));
    g.set("firstAttacker", val((int)k->first_attacker));
    g.set("defender", val((int)k->defender));
    g.set("discard", val((int)k->discard));
    if (k->has_flipped) g.set("flipped", val((int)k->flipped));
    else g.set("flipped", val::undefined());
    g.set("goodMask", val((unsigned)k->good_mask));
    g.set("hasGoodTs", val((bool)k->has_good_ts));

    val deck = val::array();
    for (int i = 0; i < k->deck_count; i++) deck.call<void>("push", (int)k->deck[i]);
    g.set("deck", deck);

    val battles = val::array();
    for (int i = 0; i < k->num_battles; i++) {
        val b = val::object();
        b.set("attack", val((int)k->battles[i].attack));
        if (k->battles[i].defense == WIRE_NONE) b.set("defense", val::undefined());
        else b.set("defense", val((int)k->battles[i].defense));
        battles.call<void>("push", b);
    }
    g.set("battles", battles);

    val players = val::array();
    for (int i = 0; i < k->num_players; i++) {
        const KPlayer *p = &k->players[i];
        val wp = val::object();
        wp.set("status", val(std::string(P_STATUS[p->status < 0 || p->status > 3 ? 0 : p->status])));
        wp.set("awaiting", val((bool)p->awaiting));
        val hand = val::array();
        for (int j = 0; j < p->hand_count; j++) hand.call<void>("push", (int)p->hand[j]);
        wp.set("hand", hand);
        players.call<void>("push", wp);
    }
    g.set("players", players);

    val elim = val::array();
    for (int i = 0; i < k->num_eliminated; i++) elim.call<void>("push", (int)k->elimination[i]);
    g.set("elimination", elim);
    return g;
}

EMSCRIPTEN_BINDINGS(kernel) {
    emscripten::function("importState", &import_state);
    emscripten::function("exportState", &export_state);
}
