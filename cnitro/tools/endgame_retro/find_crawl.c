// find_crawl (oracle/offline-only; not a shipped target). Self-play
// octogen-vs-octogen from seeds; at every octogen decision in a small
// deck-empty heads-up endgame, use the cycle-correct retrograde solver to
// classify EVERY legal move as WIN/LOSS/DRAW for the mover. Report decisions
// where exactly ONE legal move wins and all others lose ("crawl to victory").
// Build: clang -O3 -Isrc -DCD_TT_BITS=20 tools/endgame_retro/find_crawl.c <core-minus-cordite_sim> -lm
#include "cordite_sim.c"
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void og_reload_flags(void);   // FOOLISH_ORACLE_BUILD hook

enum { UNK=0, WIN=1, LOSS=2 };
enum { N_TWIN=0, N_TLOSS=1, N_TDRAW=2, N_BASE=3 };

// ---- per-decision retrograde over one endgame's reachable set --------------
#define SHB 21
#define SHS (1u<<SHB)
static uint64_t SK[SHS]; static uint32_t SV[SHS];
static uint32_t *used_slots; static long n_used;
#define RCAP 1500000
static SimState *sST; static uint8_t *sACT,*sVAL; static uint32_t *sCNT;
static long sNID; static int rovf;
static uint32_t *sEP,*sEC; static long sNE; static long SECAP=6000000;
static uint32_t *sstk; static long ssn;
static int ME;

static void two_in(const SimState*s,int*a,int*b){*a=-1;*b=-1;
    for(int i=0;i<s->num_players;i++) if(s->status_p[i]==PLAYER_STATUS_IN){ if(*a<0)*a=i; else *b=i;}}
static int actor_of(const SimState*s){ if(sim_should_act(s,s->defender)) return s->defender;
    for(int i=0;i<s->num_players;i++) if(sim_should_act(s,i)) return i; return -1; }
static long sintern(const SimState*c,int*isnew){ int a,b; two_in(c,&a,&b); uint64_t fp=sim_fingerprint(c,a,b);
    uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);
    for(;;){ if(SV[i]==0){ if(sNID>=RCAP||sNE>=SECAP-256){rovf=1;*isnew=0;return -1;}
                long id=sNID++; SK[i]=fp; SV[i]=(uint32_t)(id+1); used_slots[n_used++]=i;
                sST[id]=*c; sACT[id]=(actor_of(c)==ME); sVAL[id]=UNK; *isnew=1; return id; }
             if(SK[i]==fp){*isnew=0;return SV[i]-1;} i=(i+1)&(SHS-1); } }
// value of an arbitrary child (terminal sentinel or interned+solved)
static long classify_child(const SimState*c){ int loser=sim_done(c);
    if(loser>=0) return (loser==ME)?N_TLOSS:N_TWIN;
    if(sim_in_count(c)<2) return N_TDRAW;
    int in; long id=sintern(c,&in); if(id<0) return -1; if(in) sstk[ssn++]=(uint32_t)id; return id; }

// enumerate+solve from `root`; returns 0 ok, -1 overflow(too big/cyclic-skip)
static int retro_build(const SimState*root,int me){
    ME=me; sNID=N_BASE; sNE=0; ssn=0; rovf=0; n_used=0;
    int in; long rid=sintern(root,&in); if(rid<0) return -1; sstk[ssn++]=(uint32_t)rid;
    while(ssn>0 && !rovf){ long id=sstk[--ssn]; SimState s=sST[id]; int actor=actor_of(&s);
        SolMove mv[CD_SIM_SOLVE_MAX_MOVES]; int nm=sim_gen_moves(&s,actor,mv,CD_SIM_SOLVE_MAX_MOVES);
        for(int k=0;k<nm;k++){ SimState c; memcpy(&c,&s,offsetof(SimState,deck)); sim_apply_sol(&c,actor,&mv[k]);
            long cid=classify_child(&c); if(cid<0){rovf=1;break;} if(sNE>=SECAP){rovf=1;break;} sEP[sNE]=(uint32_t)id; sEC[sNE]=(uint32_t)cid; sNE++; } }
    if(rovf) return -1;
    // retrograde
    for(long id=0;id<sNID;id++){ sCNT[id]=0; }
    for(long e=0;e<sNE;e++) sCNT[sEP[e]]++;
    static long *rc=0,*rs=0,*fill=0,*Q=0; static uint32_t *radj=0; static long cap=0,ecap=0,qcap=0;
    if(sNID>cap){cap=sNID*2; rc=realloc(rc,cap*8); rs=realloc(rs,(cap+1)*8); fill=realloc(fill,cap*8);}
    if(sNE>ecap){ecap=sNE*2; radj=realloc(radj,ecap*4);}
    if(sNID>qcap){qcap=sNID*2; Q=realloc(Q,qcap*8);}
    for(long id=0;id<sNID;id++) rc[id]=0;
    for(long e=0;e<sNE;e++) rc[sEC[e]]++;
    rs[0]=0; for(long id=0;id<sNID;id++) rs[id+1]=rs[id]+rc[id];
    for(long id=0;id<sNID;id++) fill[id]=0;
    for(long e=0;e<sNE;e++){uint32_t c=sEC[e]; radj[rs[c]+fill[c]++]=sEP[e];}
    long qh=0,qt=0; sVAL[N_TWIN]=WIN; sVAL[N_TLOSS]=LOSS; sVAL[N_TDRAW]=UNK; Q[qh]=N_TWIN; Q[1]=N_TLOSS; qt=2;
    while(qh<qt){ long p=Q[qh++]; int v=sVAL[p];
        for(long r=rs[p];r<rs[p+1];r++){ uint32_t q=radj[r]; if(q<N_BASE||sVAL[q]!=UNK) continue;
            if(sACT[q]){ if(v==WIN){sVAL[q]=WIN;Q[qt++]=q;} else if(v==LOSS){ if(--sCNT[q]==0){sVAL[q]=LOSS;Q[qt++]=q;} } }
            else{ if(v==LOSS){sVAL[q]=LOSS;Q[qt++]=q;} else if(v==WIN){ if(--sCNT[q]==0){sVAL[q]=WIN;Q[qt++]=q;} } } } }
    return 0;
}
static void retro_clear(void){ for(long i=0;i<n_used;i++) SV[used_slots[i]]=0; n_used=0; }
// value(me) of a specific child state, after retro_build already enumerated it
static int child_value(const SimState*c){ int loser=sim_done(c);
    if(loser>=0) return (loser==ME)?LOSS:WIN; if(sim_in_count(c)<2) return UNK;
    int a,b; two_in(c,&a,&b); uint64_t fp=sim_fingerprint(c,a,b);
    uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);
    for(;;){ if(SV[i]==0) return -1; if(SK[i]==fp) return sVAL[SV[i]-1]; i=(i+1)&(SHS-1);} }

static int hexnib(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static const char*SUIT="SHCD"; static const char*RK="  6789TJQKA...";
static void cardstr(char*o,int su,int v){ // v is engine value 5..13 => rank 6..A
    static const char* R[]={"?","?","?","?","?","6","7","8","9","10","J","Q","K","A"};
    snprintf(o,8,"%s%c",(v>=5&&v<=13)?R[v]:"?",SUIT[su]); }
static void movestr(char*o,const LegalMove*m){ int w=0;
    const char*t=m->type==MOVE_ATTACK?"atk":m->type==MOVE_COVER?"cov":m->type==MOVE_PASS?"pass":m->type==MOVE_PICKUP?"pickup":m->type==MOVE_GOOD?"good":"?";
    w+=snprintf(o+w,64-w,"%s",t);
    for(int i=0;i<m->n_cards;i++){ char c[8]; cardstr(c,m->cards[i].suit,m->cards[i].value); w+=snprintf(o+w,64-w,"%s%s",i?",":":",c); } }

int main(int argc,char**argv){
    long seed0=argc>1?atol(argv[1]):1; long ncount=argc>2?atol(argv[2]):2000;
    int MAXCARDS=argc>3?atoi(argv[3]):10; int MINMOVES=argc>4?atoi(argv[4]):3;
    ensure_masks();
    used_slots=malloc(RCAP*2*sizeof(uint32_t));
    sST=malloc((size_t)RCAP*sizeof(SimState)); sACT=malloc(RCAP); sVAL=malloc(RCAP); sCNT=malloc((size_t)RCAP*4);
    sEP=malloc((size_t)SECAP*4); sEC=malloc((size_t)SECAP*4); sstk=malloc((size_t)RCAP*4);

    long hits=0, scanned=0;
    for(long sidx=seed0; sidx<seed0+ncount; sidx++){
        // derive a 32-byte seed from sidx (deterministic)
        uint8_t seed[32]; uint64_t x=(uint64_t)sidx*0x9E3779B97F4A7C15ull+0x1234567;
        for(int i=0;i<32;i++){ x^=x<<13;x^=x>>7;x^=x<<17; seed[i]=(uint8_t)(x&0xff); }
        cd_sim_solve_reset(); game_set_seed(1); game_set_deal_seed_bytes(seed,32);
        Game g; memset(&g,0,sizeof g); g.num_players=2;
        for(int i=0;i<2;i++){ g.players[i].status=PLAYER_STATUS_READY; g.players[i].strategy_key=(int8_t)STRAT_OCTOGEN;
            snprintf(g.players[i].player_id,sizeof g.players[i].player_id,"p%d",i); }
        start_game(&g);
        scanned++;
        int iters=0;
        while(game_done(&g)<0 && iters++<4000){
            int elig[MAX_PLAYERS],n_e=0; for(int i=0;i<g.num_players;i++) if(should_bot_act(&g,i)) elig[n_e++]=i;
            if(n_e==0) break;
            for(int i=n_e-1;i>0;i--){ int j=(int)(game_random()*(i+1)); if(j<0)j=0; if(j>i)j=i; int t=elig[i];elig[i]=elig[j];elig[j]=t; }
            bool acted=false;
            for(int k=0;k<n_e;k++){ int pi=elig[k]; LegalMoves moves; calculate_legal_moves(&g,pi,&moves);
                if(moves.n==0) continue;
                // ---- analysis: deck-empty heads-up small endgame? ----
                int nin=0; for(int p=0;p<g.num_players;p++) if(g.players[p].status==PLAYER_STATUS_IN) nin++;
                int total=0; for(int p=0;p<g.num_players;p++) total+=g.players[p].hand_count;
                if(g.deck_count==0 && !g.has_flipped && nin==2 && total<=MAXCARDS && moves.n>=MINMOVES){
                    SimState root; cd_sim_from_game(&root,&g);
                    if(retro_build(&root,pi)==0){
                        int nwin=0,nloss=0,ndraw=0,nunk=0,winmv=-1;
                        int val[64]; int nn=moves.n<64?moves.n:64;
                        for(int mi=0;mi<nn;mi++){ SimState c=root; if(!cd_sim_apply_root_move(&c,pi,&moves.moves[mi])){val[mi]=-9;continue;}
                            int v=child_value(&c); val[mi]=v;
                            if(v==WIN){nwin++;winmv=mi;} else if(v==LOSS)nloss++; else if(v==UNK)ndraw++; else nunk++; }
                        if(nwin==1 && nloss==(nn-1) && ndraw==0 && nunk==0){
                            hits++;
                            // non-obvious check: pure-MC octogen (no root solver,
                            // no exact-leaf rollouts) — does it pick a LOSS?
                            setenv("OG_NO_SOLVE","1",1); setenv("OG_BBLEAF","0",1); og_reload_flags();
                            int mc_idx=octogen_strategy_choose(&g,pi,&moves,NULL);
                            unsetenv("OG_NO_SOLVE"); unsetenv("OG_BBLEAF"); og_reload_flags();
                            int mc_loss = (mc_idx>=0 && mc_idx<nn && val[mc_idx]==LOSS);
                            char sx[65]; for(int i=0;i<32;i++) snprintf(sx+2*i,3,"%02x",seed[i]);
                            char wm[64]; movestr(wm,&moves.moves[winmv]);
                            char hs[160]={0}; int w=0;
                            for(int j=0;j<g.players[pi].hand_count;j++){char c[8];cardstr(c,g.players[pi].hand[j].suit,g.players[pi].hand[j].value);w+=snprintf(hs+w,160-w,"%s%s",j?" ":"",c);}
                            char oppd[64]={0}; int ow=0; int opp=pi^1;
                            for(int j=0;j<g.players[opp].hand_count;j++){char c[8];cardstr(c,g.players[opp].hand[j].suit,g.players[opp].hand[j].value);ow+=snprintf(oppd+ow,64-ow,"%s%s",j?" ":"",c);}
                            char losers[256]={0}; int lw=0;
                            for(int mi=0;mi<nn;mi++) if(val[mi]==LOSS){char mb[64];movestr(mb,&moves.moves[mi]);lw+=snprintf(losers+lw,256-lw,"%s%s",lw?" ":"",mb);}
                            printf("%s idx=%ld ply=%d seat=%d(%s) trump=%c total=%d nmoves=%d WIN=[%s] LOSE=[%s] hand=[%s] opp=[%s] mcpick=%s seed=%s\n",
                                mc_loss?"NONOBV":"HIT",sidx,g.num_logs,pi,g.defender==pi?"DEF":"ATK",SUIT[g.power_suit],total,nn,wm,losers,hs,oppd,
                                mc_loss?"LOSS":"win",sx);
                            fflush(stdout);
                        }
                    }
                    retro_clear();
                }
                int idx=octogen_strategy_choose(&g,pi,&moves,NULL);
                if(idx<0||idx>=moves.n) continue;
                const LegalMove*m=&moves.moves[idx]; bool ok=false;
                switch(m->type){ case MOVE_ATTACK: ok=handle_attack(&g,pi,m->cards,m->n_cards); break;
                    case MOVE_COVER: ok=handle_cover(&g,pi,m->cards,m->attack_cards,m->n_cards); break;
                    case MOVE_PASS: ok=handle_pass(&g,pi,m->cards,m->n_cards); break;
                    case MOVE_PICKUP: ok=handle_pickup(&g,pi); break;
                    case MOVE_GOOD: ok=handle_good(&g,pi); break; default: break; }
                if(ok){acted=true;break;} }
            if(!acted) break;
        }
    }
    fprintf(stderr,"scanned=%ld hits=%ld\n",scanned,hits);
    return 0;
}
