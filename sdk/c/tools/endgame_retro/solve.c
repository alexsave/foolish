// endgame_retro/solve.c (oracle/offline-only; not a shipped target).
// Cycle-correct exact solve of a deck-empty heads-up endgame: enumerate the
// distinct reachable positions (stored as the exact 258-byte SimState prefix) +
// edges to sentinel terminals, then run a 3-valued (WIN/LOSS/DRAW) retrograde
// fixpoint. Yields the position's TRUE game value, which alpha-beta cannot on
// cyclic lines. me = attacker = player 0. Edit A[]/D[]/power for a different
// endgame. See sdk/c/ENDGAME_CYCLES.md.
#include "cordite_sim.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { UNK=0, WIN=1, LOSS=2 };
enum { N_TWIN=0, N_TLOSS=1, N_TDRAW=2, N_BASE=3 };

typedef struct { uint8_t b[258]; } Pos;   /* exact SimState prefix (offsetof deck) */
static void pos_of(Pos*p,const SimState*s){ memcpy(p->b,s,258); }
static void state_of(SimState*s,const Pos*p){ memset(s,0,sizeof*s); memcpy(s,p->b,258); }

#define HBITS 28
#define HSIZE (1u<<HBITS)
#define HMASK (HSIZE-1u)
static uint64_t *HK; static uint32_t *HV;
static Pos *POS; static uint8_t *ACTME,*VAL; static uint32_t *CNT;
static long NID=N_BASE, CAP; static int overflow=0;
static uint32_t *STK; static long SN=0,SCAP=0;
static void spush(long id){ if(SN==SCAP){SCAP=SCAP?SCAP*2:(1L<<20);STK=realloc(STK,SCAP*4);} STK[SN++]=(uint32_t)id; }
static uint32_t *EP=NULL,*EC=NULL; static long NE=0,ECAP=0;
static void add_edge(uint32_t p,uint32_t c){ if(NE==ECAP){ECAP=ECAP?ECAP*2:(1L<<21);EP=realloc(EP,ECAP*4);EC=realloc(EC,ECAP*4);} EP[NE]=p;EC[NE]=c;NE++; }

static void two_in(const SimState*s,int*a,int*b){*a=-1;*b=-1;
    for(int i=0;i<s->num_players;i++) if(s->status_p[i]==PLAYER_STATUS_IN){ if(*a<0)*a=i; else *b=i;}}
static int actor_of(const SimState*s){ if(sim_should_act(s,s->defender)) return s->defender;
    for(int i=0;i<s->num_players;i++) if(sim_should_act(s,i)) return i; return -1; }
static long intern(const SimState*c,int*isnew){ int a,b; two_in(c,&a,&b); uint64_t fp=sim_fingerprint(c,a,b);
    uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-HBITS))&HMASK;
    for(;;){ if(HV[i]==0){ if(NID>=CAP){overflow=1;return -1;} long id=NID++; HK[i]=fp; HV[i]=(uint32_t)(id+1);
                pos_of(&POS[id],c); ACTME[id]=(actor_of(c)==0); VAL[id]=UNK; *isnew=1; return id; }
             if(HK[i]==fp){*isnew=0;return HV[i]-1;} i=(i+1)&HMASK; } }
static long classify(const SimState*c){ int loser=sim_done(c); if(loser>=0) return (loser==0)?N_TLOSS:N_TWIN;
    if(sim_in_count(c)<2) return N_TDRAW; int isnew; long id=intern(c,&isnew); if(id<0) return -1; if(isnew) spush(id); return id; }
static void make_root(SimState*s,uint64_t HA,uint64_t HD){ memset(s,0,sizeof*s); s->num_players=2; s->power_suit=1;
    s->defender=1; s->first_attacker=0; s->status=GAME_STATUS_PLAYING; s->hand[0]=HA; s->hand[1]=HD;
    s->status_p[0]=PLAYER_STATUS_IN; s->status_p[1]=PLAYER_STATUS_IN; s->in_mask=0x3u; }
static uint64_t mask_of(const int*ids,int n){uint64_t m=0;for(int i=0;i<n;i++)m|=1ull<<ids[i];return m;}

int main(int argc,char**argv){
    CAP=argc>1?atol(argv[1]):60000000L;
    int A[]={47,36,8,19,23,32,33,11,24},D[]={34,22,21};
    ensure_masks();
    HK=calloc(HSIZE,8); HV=calloc(HSIZE,4); POS=malloc((size_t)CAP*sizeof(Pos));
    ACTME=malloc(CAP); VAL=calloc(CAP,1); CNT=calloc((size_t)CAP,4);
    if(!HK||!HV||!POS||!ACTME||!VAL||!CNT){fprintf(stderr,"alloc fail\n");return 1;}
    fprintf(stderr,"alloc: POS %.2fGB hash %.2fGB\n",(double)CAP*sizeof(Pos)/1e9,(double)HSIZE*12/1e9);

    SimState root; make_root(&root,mask_of(A,9),mask_of(D,3));
    int isnew; long rid=intern(&root,&isnew); spush(rid);

    struct timespec t0,t1,t2; clock_gettime(CLOCK_MONOTONIC,&t0);
    while(SN>0 && !overflow){
        long id=STK[--SN]; SimState s; state_of(&s,&POS[id]); int actor=actor_of(&s);
        SolMove mv[CD_SIM_SOLVE_MAX_MOVES]; int nm=sim_gen_moves(&s,actor,mv,CD_SIM_SOLVE_MAX_MOVES);
        for(int k=0;k<nm;k++){ SimState c; memcpy(&c,&s,offsetof(SimState,deck)); sim_apply_sol(&c,actor,&mv[k]);
            long cid=classify(&c); if(cid<0){overflow=1;break;} add_edge((uint32_t)id,(uint32_t)cid); }
        if((NID&((1L<<21)-1))<8 && NID>(1L<<21)) fprintf(stderr,"  nodes=%ldM edges=%ldM\n",NID>>20,NE/1000000);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    printf("=== enumeration ===\ndistinct positions: %ld  edges: %ld  overflow(cap %ld): %s  (%.1fs)\n",
        NID-N_BASE,NE,CAP,overflow?"YES":"no",(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9);
    if(overflow){printf("exceeds cap; abort\n");return 0;}

    // retrograde: out-degree + reverse CSR (over all ids incl sentinels)
    for(long e=0;e<NE;e++) CNT[EP[e]]++;
    long *rc=calloc(NID,sizeof(long)); for(long e=0;e<NE;e++) rc[EC[e]]++;
    long *rs=calloc(NID+1,sizeof(long)); for(long id=0;id<NID;id++) rs[id+1]=rs[id]+rc[id];
    uint32_t *radj=malloc((size_t)NE*4); long *fill=calloc(NID,sizeof(long));
    for(long e=0;e<NE;e++){uint32_t c=EC[e]; radj[rs[c]+fill[c]++]=EP[e];} free(fill);free(rc);
    long *Q=malloc((size_t)NID*sizeof(long)); long qh=0,qt=0;
    VAL[N_TWIN]=WIN; VAL[N_TLOSS]=LOSS; VAL[N_TDRAW]=UNK; Q[qt++]=N_TWIN; Q[qt++]=N_TLOSS;
    while(qh<qt){ long p=Q[qh++]; int v=VAL[p];
        for(long r=rs[p];r<rs[p+1];r++){ uint32_t q=radj[r]; if(q<N_BASE||VAL[q]!=UNK) continue;
            if(ACTME[q]){ if(v==WIN){VAL[q]=WIN;Q[qt++]=q;} else if(v==LOSS){ if(--CNT[q]==0){VAL[q]=LOSS;Q[qt++]=q;} } }
            else{ if(v==LOSS){VAL[q]=LOSS;Q[qt++]=q;} else if(v==WIN){ if(--CNT[q]==0){VAL[q]=WIN;Q[qt++]=q;} } } } }
    long nw=0,nl=0,nd=0; for(long id=N_BASE;id<NID;id++){ if(VAL[id]==WIN)nw++; else if(VAL[id]==LOSS)nl++; else nd++; }
    clock_gettime(CLOCK_MONOTONIC,&t2);
    printf("=== retrograde ===\nWIN=%ld LOSS=%ld DRAW=%ld  (%.1fs)\n",nw,nl,nd,
        (t2.tv_sec-t1.tv_sec)+(t2.tv_nsec-t1.tv_nsec)/1e9);
    const char*rv=VAL[rid]==WIN?"WIN":VAL[rid]==LOSS?"LOSS":"DRAW";
    printf("\nROOT (octogen to move, hand 10D QC 10S 8H QH 8C 9C KS KH vs 10C JH 10H, trump H)\n");
    printf("TRUE game value (cycle-correct) = %s\n", rv);
    return 0;
}
