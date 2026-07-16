// verify_crawl (oracle/offline-only). Replay one seed's octogen-vs-octogen game
// (solver ON) to a target ply, then at that decision: (1) classify every legal
// move WIN/LOSS with the retrograde solver, (2) compare octogen's real pick
// (solver on) vs its MC-only pick (OG_NO_SOLVE=1). If MC-only picks a LOSS, the
// winning move is "non-obvious" — only the exact solver finds it.
// Compile the WHOLE program with -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD so
// og_reload_flags() is available and flags re-read between the two chooses.
#include "cordite_sim.c"
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void og_reload_flags(void);   // FOOLISH_ORACLE_BUILD hook (octogen_strategy.c)

enum { UNK=0, WIN=1, LOSS=2 };
enum { N_TWIN=0, N_TLOSS=1, N_TDRAW=2, N_BASE=3 };
#define SHB 21
#define SHS (1u<<SHB)
static uint64_t SK[SHS]; static uint32_t SV[SHS];
#define RCAP 1500000
static SimState *sST; static uint8_t *sACT,*sVAL; static uint32_t *sCNT;
static long sNID; static int rovf; static uint32_t *sEP,*sEC; static long sNE,SECAP=6000000; static uint32_t*sstk; static long ssn; static int ME;
static void two_in(const SimState*s,int*a,int*b){*a=-1;*b=-1;for(int i=0;i<s->num_players;i++)if(s->status_p[i]==PLAYER_STATUS_IN){if(*a<0)*a=i;else *b=i;}}
static int actor_of(const SimState*s){ if(sim_should_act(s,s->defender))return s->defender; for(int i=0;i<s->num_players;i++) if(sim_should_act(s,i))return i; return -1;}
static long sintern(const SimState*c,int*isnew){int a,b;two_in(c,&a,&b);uint64_t fp=sim_fingerprint(c,a,b);uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);
    for(;;){if(SV[i]==0){if(sNID>=RCAP||sNE>=SECAP-256){rovf=1;*isnew=0;return -1;}long id=sNID++;SK[i]=fp;SV[i]=(uint32_t)(id+1);sST[id]=*c;sACT[id]=(actor_of(c)==ME);sVAL[id]=UNK;*isnew=1;return id;}
        if(SK[i]==fp){*isnew=0;return SV[i]-1;}i=(i+1)&(SHS-1);}}
static long classify_child(const SimState*c){int loser=sim_done(c);if(loser>=0)return (loser==ME)?N_TLOSS:N_TWIN;if(sim_in_count(c)<2)return N_TDRAW;int in;long id=sintern(c,&in);if(id<0)return -1;if(in)sstk[ssn++]=(uint32_t)id;return id;}
static int retro_build(const SimState*root,int me){ME=me;sNID=N_BASE;sNE=0;ssn=0;rovf=0;memset(SV,0,sizeof SV);
    int in;long rid=sintern(root,&in);if(rid<0)return -1;sstk[ssn++]=(uint32_t)rid;
    while(ssn>0&&!rovf){long id=sstk[--ssn];SimState s=sST[id];int actor=actor_of(&s);SolMove mv[CD_SIM_SOLVE_MAX_MOVES];int nm=sim_gen_moves(&s,actor,mv,CD_SIM_SOLVE_MAX_MOVES);
        for(int k=0;k<nm;k++){SimState c;memcpy(&c,&s,offsetof(SimState,deck));sim_apply_sol(&c,actor,&mv[k]);long cid=classify_child(&c);if(cid<0){rovf=1;break;}if(sNE>=SECAP){rovf=1;break;}sEP[sNE]=(uint32_t)id;sEC[sNE]=(uint32_t)cid;sNE++;}}
    if(rovf)return -1;
    for(long id=0;id<sNID;id++)sCNT[id]=0; for(long e=0;e<sNE;e++)sCNT[sEP[e]]++;
    static long *rc=0,*rs=0,*fill=0,*Q=0;static uint32_t*radj=0;static long cap=0,ec=0,qc=0;
    if(sNID>cap){cap=sNID*2;rc=realloc(rc,cap*8);rs=realloc(rs,(cap+1)*8);fill=realloc(fill,cap*8);} if(sNE>ec){ec=sNE*2;radj=realloc(radj,ec*4);} if(sNID>qc){qc=sNID*2;Q=realloc(Q,qc*8);}
    for(long id=0;id<sNID;id++)rc[id]=0; for(long e=0;e<sNE;e++)rc[sEC[e]]++; rs[0]=0;for(long id=0;id<sNID;id++)rs[id+1]=rs[id]+rc[id];
    for(long id=0;id<sNID;id++)fill[id]=0; for(long e=0;e<sNE;e++){uint32_t c=sEC[e];radj[rs[c]+fill[c]++]=sEP[e];}
    long qh=0,qt=0;sVAL[N_TWIN]=WIN;sVAL[N_TLOSS]=LOSS;sVAL[N_TDRAW]=UNK;Q[0]=N_TWIN;Q[1]=N_TLOSS;qt=2;
    while(qh<qt){long p=Q[qh++];int v=sVAL[p];for(long r=rs[p];r<rs[p+1];r++){uint32_t q=radj[r];if(q<N_BASE||sVAL[q]!=UNK)continue;
        if(sACT[q]){if(v==WIN){sVAL[q]=WIN;Q[qt++]=q;}else if(v==LOSS){if(--sCNT[q]==0){sVAL[q]=LOSS;Q[qt++]=q;}}}
        else{if(v==LOSS){sVAL[q]=LOSS;Q[qt++]=q;}else if(v==WIN){if(--sCNT[q]==0){sVAL[q]=WIN;Q[qt++]=q;}}}}}
    return 0;}
static int child_value(const SimState*c){int loser=sim_done(c);if(loser>=0)return(loser==ME)?LOSS:WIN;if(sim_in_count(c)<2)return UNK;int a,b;two_in(c,&a,&b);uint64_t fp=sim_fingerprint(c,a,b);uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);for(;;){if(SV[i]==0)return -1;if(SK[i]==fp)return sVAL[SV[i]-1];i=(i+1)&(SHS-1);}}

static int hexnib(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static const char*SUIT="SHCD";
static void cardstr(char*o,int su,int v){static const char*R[]={"?","?","?","?","?","6","7","8","9","10","J","Q","K","A"};snprintf(o,8,"%s%c",(v>=5&&v<=13)?R[v]:"?",SUIT[su]);}
static void movestr(char*o,const LegalMove*m){int w=0;const char*t=m->type==MOVE_ATTACK?"atk":m->type==MOVE_COVER?"cov":m->type==MOVE_PASS?"pass":m->type==MOVE_PICKUP?"pickup":m->type==MOVE_GOOD?"good":"?";w+=snprintf(o+w,64-w,"%s",t);for(int i=0;i<m->n_cards;i++){char c[8];cardstr(c,m->cards[i].suit,m->cards[i].value);w+=snprintf(o+w,64-w,"%s%s",i?",":":",c);}}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <64hex-seed> <target_ply>\n",argv[0]);return 2;}
    uint8_t seed[32];for(int i=0;i<32;i++){int hi=hexnib(argv[1][2*i]),lo=hexnib(argv[1][2*i+1]);seed[i]=(uint8_t)((hi<<4)|lo);}
    int target=atoi(argv[2]);
    ensure_masks();
    sST=malloc((size_t)RCAP*sizeof(SimState));sACT=malloc(RCAP);sVAL=malloc(RCAP);sCNT=malloc((size_t)RCAP*4);
    sEP=malloc((size_t)SECAP*4);sEC=malloc((size_t)SECAP*4);sstk=malloc((size_t)RCAP*4);
    cd_sim_solve_reset();game_set_seed(1);game_set_deal_seed_bytes(seed,32);
    Game g;memset(&g,0,sizeof g);g.num_players=2;for(int i=0;i<2;i++){g.players[i].status=PLAYER_STATUS_READY;g.players[i].strategy_key=(int8_t)STRAT_OCTOGEN;snprintf(g.players[i].player_id,sizeof g.players[i].player_id,"p%d",i);}
    start_game(&g);
    int iters=0;
    while(game_done(&g)<0 && iters++<4000){
        int elig[MAX_PLAYERS],n_e=0;for(int i=0;i<g.num_players;i++)if(should_bot_act(&g,i))elig[n_e++]=i;if(n_e==0)break;
        for(int i=n_e-1;i>0;i--){int j=(int)(game_random()*(i+1));if(j<0)j=0;if(j>i)j=i;int t=elig[i];elig[i]=elig[j];elig[j]=t;}
        bool acted=false;
        for(int k=0;k<n_e;k++){int pi=elig[k];LegalMoves moves;calculate_legal_moves(&g,pi,&moves);if(moves.n==0)continue;
            if(g.num_logs==target){
                // ANALYSIS at the target decision
                SimState root;cd_sim_from_game(&root,&g);
                if(retro_build(&root,pi)!=0){printf("retro overflow at target\n");return 1;}
                int nn=moves.n, val[64], winmv=-1;
                printf("=== target ply %d, seat %d (%s), trump %c ===\n",target,pi,g.defender==pi?"DEFENDER":"ATTACKER",SUIT[g.power_suit]);
                char hs[160]={0};int w=0;for(int j=0;j<g.players[pi].hand_count;j++){char c[8];cardstr(c,g.players[pi].hand[j].suit,g.players[pi].hand[j].value);w+=snprintf(hs+w,160-w,"%s%s",j?" ":"",c);}
                char od[160]={0};int ow=0;int opp=pi^1;for(int j=0;j<g.players[opp].hand_count;j++){char c[8];cardstr(c,g.players[opp].hand[j].suit,g.players[opp].hand[j].value);ow+=snprintf(od+ow,160-ow,"%s%s",j?" ":"",c);}
                printf("hand=[%s]  opponent=[%s]\n",hs,od);
                for(int mi=0;mi<nn&&mi<64;mi++){SimState c=root;if(!cd_sim_apply_root_move(&c,pi,&moves.moves[mi])){val[mi]=-9;continue;}val[mi]=child_value(&c);
                    char mb[64];movestr(mb,&moves.moves[mi]);printf("  move %2d %-12s -> %s\n",mi,mb,val[mi]==WIN?"WIN":val[mi]==LOSS?"LOSS":val[mi]==UNK?"DRAW":"?");if(val[mi]==WIN)winmv=mi;}
                // solver-on pick
                unsetenv("OG_NO_SOLVE");og_reload_flags();
                int idx_solver=octogen_strategy_choose(&g,pi,&moves,NULL);
                // MC-only pick: disable the ROOT exact solver AND the exact-leaf
                // rollouts, so MC is pure heuristic (no exact endgame knowledge).
                setenv("OG_NO_SOLVE","1",1);setenv("OG_BBLEAF","0",1);og_reload_flags();
                int idx_mc=octogen_strategy_choose(&g,pi,&moves,NULL);
                unsetenv("OG_NO_SOLVE");unsetenv("OG_BBLEAF");og_reload_flags();
                char sb[64],mb[64],wb[64];movestr(sb,&moves.moves[idx_solver]);movestr(mb,&moves.moves[idx_mc]);
                if(winmv>=0)movestr(wb,&moves.moves[winmv]); else snprintf(wb,64,"?");
                printf("winning move        : %s\n",wb);
                printf("octogen (solver ON) : idx %d %-12s -> %s\n",idx_solver,sb,idx_solver<64?(val[idx_solver]==WIN?"WIN":val[idx_solver]==LOSS?"LOSS":"?"):"?");
                printf("octogen (MC only)   : idx %d %-12s -> %s  %s\n",idx_mc,mb,idx_mc<64?(val[idx_mc]==WIN?"WIN":val[idx_mc]==LOSS?"LOSS":"?"):"?",
                    (idx_mc<64&&val[idx_mc]==LOSS)?"<== NON-OBVIOUS (MC picks a LOSS; only the solver finds the win)":"(MC also finds the win — obvious)");
                return 0;
            }
            int idx=octogen_strategy_choose(&g,pi,&moves,NULL);if(idx<0||idx>=moves.n)continue;
            const LegalMove*m=&moves.moves[idx];bool ok=false;
            switch(m->type){case MOVE_ATTACK:ok=handle_attack(&g,pi,m->cards,m->n_cards);break;case MOVE_COVER:ok=handle_cover(&g,pi,m->cards,m->attack_cards,m->n_cards);break;case MOVE_PASS:ok=handle_pass(&g,pi,m->cards,m->n_cards);break;case MOVE_PICKUP:ok=handle_pickup(&g,pi);break;case MOVE_GOOD:ok=handle_good(&g,pi);break;default:break;}
            if(ok){acted=true;break;}}
        if(!acted)break;
    }
    printf("target ply %d not reached (game ended earlier)\n",target);
    return 1;
}
