// dump_game (oracle/offline-only). Play one seed's octogen-vs-octogen 2p game
// and print the exact move stream octogen chose, as JSON lines a TS driver can
// replay through the kernel to encode a v6 replay. Also prints the seed + trump.
#include "cordite_sim.c"
#include "game.h"
#include "legal.h"
#include "strategy.h"
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
    for(int i=0;i<2;i++){g.players[i].status=PLAYER_STATUS_READY;g.players[i].strategy_key=(int8_t)STRAT_OCTOGEN;snprintf(g.players[i].player_id,sizeof g.players[i].player_id,"p%d",i);}
    start_game(&g);
    printf("{\"seed\":\"%s\",\"trump_suit\":%d,\"flip\":[%d,%d],\"first_attacker\":%d,\"moves\":[\n",
        argv[1],g.power_suit,g.flipped.suit,g.flipped.value,g.first_attacker);
    int iters=0,first=1;
    while(game_done(&g)<0 && iters++<4000){
        int elig[MAX_PLAYERS],n_e=0;for(int i=0;i<g.num_players;i++)if(should_bot_act(&g,i))elig[n_e++]=i;if(n_e==0)break;
        for(int i=n_e-1;i>0;i--){int j=(int)(game_random()*(i+1));if(j<0)j=0;if(j>i)j=i;int t=elig[i];elig[i]=elig[j];elig[j]=t;}
        bool acted=false;
        for(int k=0;k<n_e;k++){int pi=elig[k];LegalMoves moves;calculate_legal_moves(&g,pi,&moves);if(moves.n==0)continue;
            int ply=g.num_logs;
            int idx=octogen_strategy_choose(&g,pi,&moves,NULL);if(idx<0||idx>=moves.n)continue;
            const LegalMove*m=&moves.moves[idx];
            printf("%s{\"ply\":%d,\"seat\":%d,\"type\":\"%s\",\"cards\":[",first?"":",\n",ply,pi,tname(m->type));
            for(int j=0;j<m->n_cards;j++)printf("%s[%d,%d]",j?",":"",m->cards[j].suit,m->cards[j].value);
            printf("]");
            if(m->type==MOVE_COVER){printf(",\"attack\":[");for(int j=0;j<m->n_cards;j++)printf("%s[%d,%d]",j?",":"",m->attack_cards[j].suit,m->attack_cards[j].value);printf("]");}
            printf("}");first=0;
            bool ok=false;
            switch(m->type){case MOVE_ATTACK:ok=handle_attack(&g,pi,m->cards,m->n_cards);break;case MOVE_COVER:ok=handle_cover(&g,pi,m->cards,m->attack_cards,m->n_cards);break;case MOVE_PASS:ok=handle_pass(&g,pi,m->cards,m->n_cards);break;case MOVE_PICKUP:ok=handle_pickup(&g,pi);break;case MOVE_GOOD:ok=handle_good(&g,pi);break;default:break;}
            if(ok){acted=true;break;}}
        if(!acted)break;
    }
    printf("\n],\"fool\":%d,\"num_logs\":%d}\n",game_done(&g),g.num_logs);
    (void)TN;
    return 0;
}
