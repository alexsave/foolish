// narrow_path (oracle/offline-only). Self-play octogen-vs-octogen; for the seat
// that WINS, measure how NARROW its proven winning line is: at each of its own
// deck-empty endgame decisions that offers a real choice (>=2 legal moves), use
// the retrograde solver to count how many moves WIN. A run of consecutive such
// decisions each with exactly ONE winning move = a tightrope the winner must
// walk perfectly. Reports, per seed, the winner's longest forced run.
// Needs -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD only if you add MC checks; the
// base build (plain) suffices here.
#include "cordite_sim.c"
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { UNK=0, WIN=1, LOSS=2 };
enum { N_TWIN=0, N_TLOSS=1, N_TDRAW=2, N_BASE=3 };
#define SHB 21
#define SHS (1u<<SHB)
static uint64_t SK[SHS]; static uint32_t SV[SHS]; static uint32_t *used_slots; static long n_used;
#define RCAP 1500000
static SimState *sST; static uint8_t *sACT,*sVAL; static uint32_t *sCNT; static long sNID; static int rovf;
static uint32_t *sEP,*sEC; static long sNE,SECAP=6000000; static uint32_t*sstk; static long ssn; static int ME;
static void two_in(const SimState*s,int*a,int*b){*a=-1;*b=-1;for(int i=0;i<s->num_players;i++)if(s->status_p[i]==PLAYER_STATUS_IN){if(*a<0)*a=i;else *b=i;}}
static int actor_of(const SimState*s){if(sim_should_act(s,s->defender))return s->defender;for(int i=0;i<s->num_players;i++)if(sim_should_act(s,i))return i;return -1;}
static long sintern(const SimState*c,int*isnew){int a,b;two_in(c,&a,&b);uint64_t fp=sim_fingerprint(c,a,b);uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);
    for(;;){if(SV[i]==0){if(sNID>=RCAP||sNE>=SECAP-256){rovf=1;*isnew=0;return -1;}long id=sNID++;SK[i]=fp;SV[i]=(uint32_t)(id+1);used_slots[n_used++]=i;sST[id]=*c;sACT[id]=(actor_of(c)==ME);sVAL[id]=UNK;*isnew=1;return id;}
        if(SK[i]==fp){*isnew=0;return SV[i]-1;}i=(i+1)&(SHS-1);}}
static long classify_child(const SimState*c){int loser=sim_done(c);if(loser>=0)return (loser==ME)?N_TLOSS:N_TWIN;if(sim_in_count(c)<2)return N_TDRAW;int in;long id=sintern(c,&in);if(id<0)return -1;if(in)sstk[ssn++]=(uint32_t)id;return id;}
static int retro_build(const SimState*root,int me){ME=me;sNID=N_BASE;sNE=0;ssn=0;rovf=0;n_used=0;
    int in;long rid=sintern(root,&in);if(rid<0)return -1;sstk[ssn++]=(uint32_t)rid;
    while(ssn>0&&!rovf){long id=sstk[--ssn];SimState s=sST[id];int actor=actor_of(&s);SolMove mv[CD_SIM_SOLVE_MAX_MOVES];int nm=sim_gen_moves(&s,actor,mv,CD_SIM_SOLVE_MAX_MOVES);
        for(int k=0;k<nm;k++){SimState c;memcpy(&c,&s,offsetof(SimState,deck));sim_apply_sol(&c,actor,&mv[k]);long cid=classify_child(&c);if(cid<0){rovf=1;break;}if(sNE>=SECAP){rovf=1;break;}sEP[sNE]=(uint32_t)id;sEC[sNE]=(uint32_t)cid;sNE++;}}
    if(rovf)return -1;
    for(long id=0;id<sNID;id++)sCNT[id]=0;for(long e=0;e<sNE;e++)sCNT[sEP[e]]++;
    static long *rc=0,*rs=0,*fill=0,*Q=0;static uint32_t*radj=0;static long cap=0,ec=0,qc=0;
    if(sNID>cap){cap=sNID*2;rc=realloc(rc,cap*8);rs=realloc(rs,(cap+1)*8);fill=realloc(fill,cap*8);}if(sNE>ec){ec=sNE*2;radj=realloc(radj,ec*4);}if(sNID>qc){qc=sNID*2;Q=realloc(Q,qc*8);}
    for(long id=0;id<sNID;id++)rc[id]=0;for(long e=0;e<sNE;e++)rc[sEC[e]]++;rs[0]=0;for(long id=0;id<sNID;id++)rs[id+1]=rs[id]+rc[id];
    for(long id=0;id<sNID;id++)fill[id]=0;for(long e=0;e<sNE;e++){uint32_t c=sEC[e];radj[rs[c]+fill[c]++]=sEP[e];}
    long qh=0,qt=0;sVAL[N_TWIN]=WIN;sVAL[N_TLOSS]=LOSS;sVAL[N_TDRAW]=UNK;Q[0]=N_TWIN;Q[1]=N_TLOSS;qt=2;
    while(qh<qt){long p=Q[qh++];int v=sVAL[p];for(long r=rs[p];r<rs[p+1];r++){uint32_t q=radj[r];if(q<N_BASE||sVAL[q]!=UNK)continue;
        if(sACT[q]){if(v==WIN){sVAL[q]=WIN;Q[qt++]=q;}else if(v==LOSS){if(--sCNT[q]==0){sVAL[q]=LOSS;Q[qt++]=q;}}}
        else{if(v==LOSS){sVAL[q]=LOSS;Q[qt++]=q;}else if(v==WIN){if(--sCNT[q]==0){sVAL[q]=WIN;Q[qt++]=q;}}}}}
    return 0;}
static void retro_clear(void){for(long i=0;i<n_used;i++)SV[used_slots[i]]=0;n_used=0;}
static int child_value(const SimState*c){int loser=sim_done(c);if(loser>=0)return(loser==ME)?LOSS:WIN;if(sim_in_count(c)<2)return UNK;int a,b;two_in(c,&a,&b);uint64_t fp=sim_fingerprint(c,a,b);uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);for(;;){if(SV[i]==0)return -1;if(SK[i]==fp)return sVAL[SV[i]-1];i=(i+1)&(SHS-1);}}

static uint32_t seedx;
static void mkseed(long idx,uint8_t*seed){uint64_t x=(uint64_t)idx*0x9E3779B97F4A7C15ull+0x1234567;for(int i=0;i<32;i++){x^=x<<13;x^=x>>7;x^=x<<17;seed[i]=(uint8_t)(x&0xff);}}

int main(int argc,char**argv){
    long seed0=argc>1?atol(argv[1]):1, ncount=argc>2?atol(argv[2]):2000;
    int MAXCARDS=argc>3?atoi(argv[3]):11, MINRUN=argc>4?atoi(argv[4]):3, MINMOVES=argc>5?atoi(argv[5]):2;
    ensure_masks();
    used_slots=malloc(RCAP*2*sizeof(uint32_t));sST=malloc((size_t)RCAP*sizeof(SimState));sACT=malloc(RCAP);sVAL=malloc(RCAP);sCNT=malloc((size_t)RCAP*4);
    sEP=malloc((size_t)SECAP*4);sEC=malloc((size_t)SECAP*4);sstk=malloc((size_t)RCAP*4);
    long scanned=0,reported=0;
    for(long sidx=seed0;sidx<seed0+ncount;sidx++){
        uint8_t seed[32];mkseed(sidx,seed);
        cd_sim_solve_reset();game_set_seed(1);game_set_deal_seed_bytes(seed,32);
        Game g;memset(&g,0,sizeof g);g.num_players=2;for(int i=0;i<2;i++){g.players[i].status=PLAYER_STATUS_READY;g.players[i].strategy_key=(int8_t)STRAT_OCTOGEN;snprintf(g.players[i].player_id,sizeof g.players[i].player_id,"p%d",i);}
        start_game(&g);scanned++;
        int run[2]={0,0},maxrun[2]={0,0},maxstart[2]={-1,-1},runstart[2]={-1,-1};
        int iters=0;
        while(game_done(&g)<0 && iters++<4000){
            int elig[MAX_PLAYERS],n_e=0;for(int i=0;i<g.num_players;i++)if(should_bot_act(&g,i))elig[n_e++]=i;if(n_e==0)break;
            for(int i=n_e-1;i>0;i--){int j=(int)(game_random()*(i+1));if(j<0)j=0;if(j>i)j=i;int t=elig[i];elig[i]=elig[j];elig[j]=t;}
            bool acted=false;
            for(int k=0;k<n_e;k++){int pi=elig[k];LegalMoves moves;calculate_legal_moves(&g,pi,&moves);if(moves.n==0)continue;
                int nin=0;for(int p=0;p<g.num_players;p++)if(g.players[p].status==PLAYER_STATUS_IN)nin++;
                int total=0;for(int p=0;p<g.num_players;p++)total+=g.players[p].hand_count;
                int idx=octogen_strategy_choose(&g,pi,&moves,NULL);if(idx<0||idx>=moves.n)continue;
                if(g.deck_count==0&&!g.has_flipped&&nin==2&&total<=MAXCARDS&&moves.n>=MINMOVES){
                    SimState root;cd_sim_from_game(&root,&g);
                    if(retro_build(&root,pi)==0){
                        int nwin=0; int playedwin=0;
                        for(int mi=0;mi<moves.n;mi++){SimState c=root;if(!cd_sim_apply_root_move(&c,pi,&moves.moves[mi]))continue;int v=child_value(&c);if(v==WIN){nwin++;if(mi==idx)playedwin=1;}}
                        if(nwin==1 && playedwin){ if(run[pi]==0)runstart[pi]=g.num_logs; run[pi]++; if(run[pi]>maxrun[pi]){maxrun[pi]=run[pi];maxstart[pi]=runstart[pi];} }
                        else { run[pi]=0; }   // a non-unique-win (or losing) real choice breaks the tightrope
                    }
                    retro_clear();
                }
                const LegalMove*m=&moves.moves[idx];bool ok=false;
                switch(m->type){case MOVE_ATTACK:ok=handle_attack(&g,pi,m->cards,m->n_cards);break;case MOVE_COVER:ok=handle_cover(&g,pi,m->cards,m->attack_cards,m->n_cards);break;case MOVE_PASS:ok=handle_pass(&g,pi,m->cards,m->n_cards);break;case MOVE_PICKUP:ok=handle_pickup(&g,pi);break;case MOVE_GOOD:ok=handle_good(&g,pi);break;default:break;}
                if(ok){acted=true;break;}}
            if(!acted)break;
        }
        int fool=game_done(&g);            // the loser seat (heads-up); winner = 1-fool
        if(fool<0)continue;
        int win_seat=1-fool;
        if(maxrun[win_seat]>=MINRUN){
            reported++;
            char sx[65];for(int i=0;i<32;i++)snprintf(sx+2*i,3,"%02x",seed[i]);
            printf("NARROW idx=%ld winner=%d forced_run=%d run_start_ply=%d fool=%d seed=%s\n",
                sidx,win_seat,maxrun[win_seat],maxstart[win_seat],fool,sx);
            fflush(stdout);
        }
    }
    fprintf(stderr,"scanned=%ld reported=%ld\n",scanned,reported);
    (void)seedx;
    return 0;
}
