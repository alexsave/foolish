// fixture_admin_panels.h - the console's games, game and bots answers,
// byte for byte, for the inputs unit_admin_panels.c builds (a kernel-made lobby
// of MAX_PLAYERS, a roster with an unknown format byte, a game with no roster,
// one browsed but gone by the read, three bots).
//
// WHERE THEY CAME FROM (2026-10-18): the pre-split binary, platform-migration
// at 7c5cb85da (the commit before stage 7 moved the panels out of
// c_base/src/admin/admin.c), served the same inputs through cb_handle, and
// these are its answers. The split code (foolish/server/src/app/admin_panels.c
// over core_json.c's plain structs) must answer the same bytes: the console's
// HTTP bodies are client-visible.
#ifndef CB_FIXTURE_ADMIN_PANELS_H
#define CB_FIXTURE_ADMIN_PANELS_H

typedef struct {
    const char *name;    // the case: games, game, game_bad, game_none, bots
    int         status;
    const char *body;
} admin_panel_golden;

static const admin_panel_golden ADMIN_PANEL_GOLDEN[] = {
    { "games", 200,
      "{\"games\":[{\"id\":\"f00d01\",\"status\":0,\"status_label\":\"waiting\",\"version\":7,\"round_epoch\":2,\"needs_bots\":true"
      ",\"created_at\":\"2025-10-09T08:55:18.456000+00:00\",\"updated_at\":\"2025-10-09T08:55:23.446000+00:00\","
      "\"last_commit_at\":\"2025-10-09T08:55:23.436000+00:00\",\"seats\":[{\"name\":\"H\303\264st \\\"H\\\"\",\"id\":\"11111111-1111-4"
      "111-8111-111111111111\",\"bot\":false},{\"name\":\"back\\\\slash\",\"id\":\"22222222-2222-4222-8222-222222222222\","
      "\"bot\":false},{\"name\":\"\320\236\320\273\321\217\",\"id\":\"33333333-3333-4333-8333-333333333333\",\"bot\":false},{\"name\":\"dee\","
      "\"id\":\"44444444-4444-4444-8444-444444444444\",\"bot\":false},{\"name\":\"%Miami 1\",\"id\":\"2c601cde-63d2-5291-8fc"
      "b-735f5860b1ec\",\"bot\":true},{\"name\":\"%Miami 2\",\"id\":\"98d81d2b-94d3-540f-a5c9-106293670763\",\"bot\":true},"
      "{\"name\":\"%New York 1\",\"id\":\"5847bff1-2330-534e-87f3-19af55d66e60\",\"bot\":true},{\"name\":\"%Seoul 1\","
      "\"id\":\"3e2ad143-95b7-58c9-bbce-e46f6fc0b545\",\"bot\":true}],\"title\":\"H\303\264st \\\"H\\\"'s Game\"},{\"id\":\"f00d02\","
      "\"status\":1,\"status_label\":\"playing\",\"version\":3,\"round_epoch\":0,\"needs_bots\":false,\"created_at\":\"1970-01"
      "-01T00:00:00.001000+00:00\",\"updated_at\":\"1970-01-01T00:00:00.002000+00:00\",\"last_commit_at\":null,"
      "\"seats\":null,\"roster_error\":-2},{\"id\":\"f00d03\",\"status\":2,\"status_label\":\"game_over\",\"version\":1,"
      "\"round_epoch\":-1,\"needs_bots\":false,\"created_at\":null,\"updated_at\":null,\"last_commit_at\":null,\"seats\":null,"
      "\"roster_error\":-1}],\"next\":\"next\\\"page\"}" },
    { "game", 200,
      "{\"id\":\"f00d01\",\"status\":0,\"status_label\":\"waiting\",\"version\":7,\"round_epoch\":2,\"needs_bots\":true,"
      "\"created_at\":\"2025-10-09T08:55:18.456000+00:00\",\"updated_at\":\"2025-10-09T08:55:23.446000+00:00\","
      "\"last_commit_at\":\"2025-10-09T08:55:23.436000+00:00\",\"seats\":[{\"name\":\"H\303\264st \\\"H\\\"\",\"id\":\"11111111-1111-4"
      "111-8111-111111111111\",\"bot\":false},{\"name\":\"back\\\\slash\",\"id\":\"22222222-2222-4222-8222-222222222222\","
      "\"bot\":false},{\"name\":\"\320\236\320\273\321\217\",\"id\":\"33333333-3333-4333-8333-333333333333\",\"bot\":false},{\"name\":\"dee\","
      "\"id\":\"44444444-4444-4444-8444-444444444444\",\"bot\":false},{\"name\":\"%Miami 1\",\"id\":\"2c601cde-63d2-5291-8fc"
      "b-735f5860b1ec\",\"bot\":true},{\"name\":\"%Miami 2\",\"id\":\"98d81d2b-94d3-540f-a5c9-106293670763\",\"bot\":true},"
      "{\"name\":\"%New York 1\",\"id\":\"5847bff1-2330-534e-87f3-19af55d66e60\",\"bot\":true},{\"name\":\"%Seoul 1\","
      "\"id\":\"3e2ad143-95b7-58c9-bbce-e46f6fc0b545\",\"bot\":true}],\"title\":\"H\303\264st \\\"H\\\"'s Game\",\"state_len\":105,"
      "\"roster_len\":1227,\"logs_len\":3,\"has_seed\":true,\"envelope_hex\":\"0102ff0700000000002c0001ff000800ffff00000"
      "0fe000000000000000000000000000000000000000001000001000001000001000000010600663030643031100048c3b47374202"
      "2482227732047616d650008000948c3b4737420224822010a6261636b5c736c6173680206d09ed0bbd18f03036465650408254d6"
      "9616d6920310508254d69616d692032060b254e657720596f726b203107082553656f756c2031240031313131313131312d31313"
      "1312d343131312d383131312d31313131313131313131313100240032323232323232322d323232322d343232322d383232322d3"
      "2323232323232323232323200240033333333333333332d333333332d343333332d383333332d333333333333333333333333002"
      "40034343434343434342d343434342d343434342d383434342d34343434343434343434343400240032633630316364652d36336"
      "4322d353239312d386663622d37333566353836306231656301240039386438316432622d393464332d353430662d613563392d3"
      "1303632393336373037363301240035383437626666312d323333302d353334652d383766332d313961663535643636653630012"
      "40033653261643134332d393562372d353863392d626263652d653436663666633062353435010000\",\"envelope_version\":7}" },
    { "game_bad", 200,
      "{\"id\":\"f00d02\",\"status\":1,\"status_label\":\"playing\",\"version\":3,\"round_epoch\":0,\"needs_bots\":false,"
      "\"created_at\":\"1970-01-01T00:00:00.001000+00:00\",\"updated_at\":\"1970-01-01T00:00:00.002000+00:00\","
      "\"last_commit_at\":null,\"seats\":null,\"roster_error\":-2,\"state_len\":4,\"roster_len\":1227,\"logs_len\":0,"
      "\"has_seed\":false,\"envelope_hex\":null,\"envelope_error\":\"CB_E_STATE\"}" },
    { "game_none", 200,
      "{\"id\":\"f00d03\",\"status\":2,\"status_label\":\"game_over\",\"version\":1,\"round_epoch\":-1,\"needs_bots\":false,"
      "\"created_at\":null,\"updated_at\":null,\"last_commit_at\":null,\"seats\":null,\"roster_error\":-1,\"state_len\":0,"
      "\"roster_len\":0,\"logs_len\":0,\"has_seed\":false,\"envelope_hex\":null,\"envelope_error\":\"CB_E_STATE\"}" },
    { "bots", 200,
      "{\"bots\":[{\"id\":\"2c601cde-63d2-5291-8fcb-735f5860b1ec\",\"nickname\":\"%Miami 1\",\"strategy_key\":\"random\","
      "\"elo_rating\":1000,\"previous_elo\":null,\"games_played\":0,\"updated_at\":null},{\"id\":\"6127a054-561f-5354-adac"
      "-db89739d26a7\",\"nickname\":\"%Q\\\"uo\\\\te \303\251\",\"strategy_key\":\"cordite\",\"elo_rating\":1873,\"previous_elo\":1860"
      ",\"games_played\":412,\"updated_at\":\"2025-10-09T08:55:23.456000+00:00\"},{\"id\":\"4b6ecbc2-45cf-5a0f-be01-4eb7"
      "16ffb6c0\",\"nickname\":\"%Moscow 1\",\"strategy_key\":\"octogen\",\"elo_rating\":0,\"previous_elo\":2400,\"games_playe"
      "d\":1,\"updated_at\":null}]}" },
};
#define ADMIN_PANEL_GOLDEN_N (int)(sizeof ADMIN_PANEL_GOLDEN / sizeof ADMIN_PANEL_GOLDEN[0])

#endif
