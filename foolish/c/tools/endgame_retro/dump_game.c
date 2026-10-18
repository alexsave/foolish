// dump_game (oracle/offline-only). Play one seed's octogen-vs-octogen 2p game
// and print the exact move stream octogen chose, as JSON lines a TS driver can
// replay through the kernel to encode a v6 replay. Also prints the seed + trump.
#include "cordite_sim.c"
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "bot_play.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexnib(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static const char*TN[]={"attack","cover","pass","pickup","good","?"};
static const char* tname(int t){switch(t){case MOVE_ATTACK:return"attack";case MOVE_COVER:return"cover";case MOVE_PASS:return"pass";case MOVE_PICKUP:return"pickup";case MOVE_GOOD:return"good";}return"?";}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <64hex-seed>\n",argv[0]);return 2;}
    uint8_t seed[32];for(int i=0;i<32;i++){int hi=hexnib(argv[1][2*i]),lo=hexnib(argv[1][2*i+1]);seed[i]=(uint8_t)((hi<<4)|lo);}
    ensure_masks();
    cd_sim_solve_reset();game_set_seed(1);game_set_deal_seed_bytes(seed,32);
    Game g;memset(&g,0,sizeof g);g.num_players=2;
    for(int i=0;i<2;i++){g.players[i].status=PLAYER_STATUS_READY;g.players[i].strategy_key=(int8_t)STRAT_OCTOGEN;}
    start_game(&g);
    printf("{\"seed\":\"%s\",\"trump_suit\":%d,\"flip\":[%d,%d],\"first_attacker\":%d,\"moves\":[\n",
        argv[1],g.power_suit,g.flipped.suit,g.flipped.value,g.first_attacker);
    int first=1;
    BotPlay bp;bot_play_init(&bp,&g,4000,BOT_PLAY_SHUFFLE,NULL);
    for(int pi;(pi=bot_play_next(&bp))>=0;){const LegalMoves*mv=&bp.moves;
        int ply=g.num_logs;
        int idx=octogen_strategy_choose(&g,pi,mv,NULL);if(idx<0||idx>=mv->n)continue;
        const LegalMove*m=&mv->moves[idx];
        printf("%s{\"ply\":%d,\"seat\":%d,\"type\":\"%s\",\"cards\":[",first?"":",\n",ply,pi,tname(m->type));
        for(int j=0;j<m->n_cards;j++)printf("%s[%d,%d]",j?",":"",m->cards[j].suit,m->cards[j].value);
        printf("]");
        if(m->type==MOVE_COVER){printf(",\"attack\":[");for(int j=0;j<m->n_cards;j++)printf("%s[%d,%d]",j?",":"",m->attack_cards[j].suit,m->attack_cards[j].value);printf("]");}
        printf("}");first=0;
        bot_play_apply(&bp,idx);
    }
    printf("\n],\"fool\":%d,\"num_logs\":%d}\n",game_done(&g),g.num_logs);
    (void)TN;
    return 0;
}
