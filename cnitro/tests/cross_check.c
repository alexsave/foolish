// C side of the cross-check (see cross_check_ts.ts). Builds the exact same
// game state and prints the same token sequence. The two outputs should be
// identical line-for-line.

#include "../src/game.h"
#include "../src/tokenize.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 2;
    g.status = GAME_STATUS_PLAYING;
    g.power_suit = 3;
    g.first_attacker = 0; g.defender = 1;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    snprintf(g.players[0].player_id, sizeof(g.players[0].player_id), "p0");
    snprintf(g.players[1].player_id, sizeof(g.players[1].player_id), "p1");

    Card p0[4] = { {0,5},{1,7},{2,9},{3,12} };
    Card p1[4] = { {0,8},{1,6},{2,10},{3,11} };
    for (int i = 0; i < 4; i++) g.players[0].hand[i] = p0[i];
    for (int i = 0; i < 4; i++) g.players[1].hand[i] = p1[i];
    g.players[0].hand_count = 4;
    g.players[1].hand_count = 4;

    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ 0, 5 };
    g.table_battles[0].has_defense = false;

    g.deck_count = 0;
    g.has_flipped = false;

    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized t;
    tokenize(&g, 0, &ip, &t);

    printf("tokens.length=%d\n", t.n_tokens);
    printf("tokens=");
    for (int i = 0; i < t.n_tokens; i++) {
        if (i > 0) printf(",");
        printf("%d", t.tokens[i]);
    }
    printf("\n");
    printf("vocab_size=%d num_actions=%d\n", VOCAB_SIZE, NUM_ACTIONS);
    return 0;
}
