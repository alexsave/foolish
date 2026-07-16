// endgame_retro/validate.c (oracle/offline-only; not a shipped target).
// Validate the retrograde 3-valued solver against the alpha-beta solver on
// SMALL deck-empty endgames that alpha-beta resolves exactly (aborted=0). For
// those, sign(alpha-beta value) must equal retrograde WIN/LOSS/DRAW. Any
// disagreement means one solver is wrong. me = attacker = player 0.
// See sdk/c/ENDGAME_CYCLES.md.
#include "cordite_sim.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { UNK=0, WIN=1, LOSS=2 };
enum { N_TWIN=0, N_TLOSS=1, N_TDRAW=2, N_BASE=3 };

// small per-call retrograde over the reachable set of ONE endgame.
#define SHB 22
#define SHS (1u<<SHB)
static uint64_t SK[SHS]; static uint32_t SV[SHS];
static SimState *sST; static uint8_t *sACT,*sVAL; static uint32_t *sCNT;
static long sNID, sCAP=4000000;
static uint32_t *sEP,*sEC; static long sNE,sECAP=8000000;
static uint32_t *sstk; static long ssn;

static void two_in(const SimState*s,int*a,int*b){*a=-1;*b=-1;
    for(int i=0;i<s->num_players;i++) if(s->status_p[i]==PLAYER_STATUS_IN){ if(*a<0)*a=i; else *b=i;}}
static int actor_of(const SimState*s){ if(sim_should_act(s,s->defender)) return s->defender;
    for(int i=0;i<s->num_players;i++) if(sim_should_act(s,i)) return i; return -1; }
static long sintern(const SimState*c,int*isnew){ int a,b; two_in(c,&a,&b); uint64_t fp=sim_fingerprint(c,a,b);
    uint32_t i=(uint32_t)((fp*0x9E3779B97F4A7C15ull)>>(64-SHB))&(SHS-1);
    for(;;){ if(SV[i]==0){ long id=sNID++; SK[i]=fp; SV[i]=(uint32_t)(id+1); sST[id]=*c;
                sACT[id]=(actor_of(c)==0); sVAL[id]=UNK; *isnew=1; return id; }
             if(SK[i]==fp){*isnew=0;return SV[i]-1;} i=(i+1)&(SHS-1); } }
static long sclassify(const SimState*c){ int loser=sim_done(c); if(loser>=0) return (loser==0)?N_TLOSS:N_TWIN;
    if(sim_in_count(c)<2) return N_TDRAW; int in; long id=sintern(c,&in); if(in){ sstk[ssn++]=(uint32_t)id; } return id; }

// returns WIN/LOSS/DRAW for attacker(player0) to move at (HA,HD,power).
static int retro(uint64_t HA,uint64_t HD,int power){
    memset(SV,0,sizeof(SV)); sNID=N_BASE; sNE=0; ssn=0;
    SimState root; memset(&root,0,sizeof root); root.num_players=2; root.power_suit=(uint8_t)power;
    root.defender=1; root.first_attacker=0; root.status=GAME_STATUS_PLAYING; root.hand[0]=HA; root.hand[1]=HD;
    root.status_p[0]=PLAYER_STATUS_IN; root.status_p[1]=PLAYER_STATUS_IN; root.in_mask=0x3u;
    int in; long rid=sintern(&root,&in); sstk[ssn++]=(uint32_t)rid;
    while(ssn>0){ long id=sstk[--ssn]; SimState s=sST[id]; int actor=actor_of(&s);
        SolMove mv[CD_SIM_SOLVE_MAX_MOVES]; int nm=sim_gen_moves(&s,actor,mv,CD_SIM_SOLVE_MAX_MOVES);
        for(int k=0;k<nm;k++){ SimState c; memcpy(&c,&s,offsetof(SimState,deck)); sim_apply_sol(&c,actor,&mv[k]);
            long cid=sclassify(&c); sEP[sNE]=(uint32_t)id; sEC[sNE]=(uint32_t)cid; sNE++; } }
    // retrograde
    for(long id=0;id<sNID;id++) sCNT[id]=0;
    for(long e=0;e<sNE;e++) sCNT[sEP[e]]++;
    static long *rc=0,*rs=0,*fill=0; static uint32_t *radj=0; static long cap=0;
    if(sNID>cap){ cap=sNID*2; rc=realloc(rc,cap*sizeof(long)); rs=realloc(rs,(cap+1)*sizeof(long)); fill=realloc(fill,cap*sizeof(long)); }
    static long ecap=0; if(sNE>ecap){ ecap=sNE*2; radj=realloc(radj,ecap*4);}
    for(long id=0;id<sNID;id++) rc[id]=0;
    for(long e=0;e<sNE;e++) rc[sEC[e]]++;
    rs[0]=0; for(long id=0;id<sNID;id++) rs[id+1]=rs[id]+rc[id];
    for(long id=0;id<sNID;id++) fill[id]=0;
    for(long e=0;e<sNE;e++){uint32_t c=sEC[e]; radj[rs[c]+fill[c]++]=sEP[e];}
    static long *Q=0; static long qcap=0; if(sNID>qcap){qcap=sNID*2; Q=realloc(Q,qcap*sizeof(long));}
    long qh=0,qt=0; sVAL[N_TWIN]=WIN; sVAL[N_TLOSS]=LOSS; sVAL[N_TDRAW]=UNK; Q[qt++]=N_TWIN; Q[qt++]=N_TLOSS;
    while(qh<qt){ long p=Q[qh++]; int v=sVAL[p];
        for(long r=rs[p];r<rs[p+1];r++){ uint32_t q=radj[r]; if(q<N_BASE||sVAL[q]!=UNK) continue;
            if(sACT[q]){ if(v==WIN){sVAL[q]=WIN;Q[qt++]=q;} else if(v==LOSS){ if(--sCNT[q]==0){sVAL[q]=LOSS;Q[qt++]=q;} } }
            else{ if(v==LOSS){sVAL[q]=LOSS;Q[qt++]=q;} else if(v==WIN){ if(--sCNT[q]==0){sVAL[q]=WIN;Q[qt++]=q;} } } } }
    return sVAL[rid];
}

static int ab_value(uint64_t HA,uint64_t HD,int power,int*aborted){
    SimState s; memset(&s,0,sizeof s); s.num_players=2; s.power_suit=(uint8_t)power;
    s.defender=1; s.first_attacker=0; s.status=GAME_STATUS_PLAYING; s.hand[0]=HA; s.hand[1]=HD;
    s.status_p[0]=PLAYER_STATUS_IN; s.status_p[1]=PLAYER_STATUS_IN; s.in_mask=0x3u;
    cd_sim_solve_reset(); return cd_sim_solve(&s,0,-1001,1001,500000000L,aborted);
}

// deck: 36-card (ids where value 5..13, i.e. offset 4..12 within each suit)
static int DECK[36], NDECK;
static uint32_t rng=0x12345678u; static uint32_t rnd(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}

int main(int argc,char**argv){
    int NCASE=argc>1?atoi(argv[1]):20000;
    int NCARDS=argc>2?atoi(argv[2]):4;   // total cards split between two hands
    ensure_masks();
    sST=malloc((size_t)sCAP*sizeof(SimState)); sACT=malloc(sCAP); sVAL=malloc(sCAP); sCNT=malloc((size_t)sCAP*4);
    sEP=malloc((size_t)sECAP*4); sEC=malloc((size_t)sECAP*4); sstk=malloc((size_t)sCAP*4);
    NDECK=0; for(int su=0;su<4;su++) for(int v=5;v<=13;v++) DECK[NDECK++]=su*13+(v-1);

    long agree=0,disagree=0,skip=0;
    for(int t=0;t<NCASE;t++){
        // pick NCARDS distinct deck cards, split first half attacker, rest defender
        int idx[12]; int chosen=0; uint64_t used=0;
        while(chosen<NCARDS){ int c=DECK[rnd()%NDECK]; if(used>>c&1) continue; used|=1ull<<c; idx[chosen++]=c; }
        int na=1+rnd()%(NCARDS-1);  // attacker gets 1..NCARDS-1
        uint64_t HA=0,HD=0; for(int i=0;i<NCARDS;i++){ if(i<na) HA|=1ull<<idx[i]; else HD|=1ull<<idx[i]; }
        if(!HA||!HD) { skip++; continue; }
        int power=rnd()%4;
        int aborted=0; int v=ab_value(HA,HD,power,&aborted);
        if(aborted){ skip++; continue; }               // only compare fully-resolved cases
        int abr = v>0?WIN : v<0?LOSS : UNK/*draw*/;
        int rr = retro(HA,HD,power); if(rr==UNK) rr=UNK;  // DRAW
        int rsign = (rr==WIN)?WIN:(rr==LOSS)?LOSS:UNK;
        if(rsign==abr) agree++;
        else { disagree++;
            if(disagree<=15) printf("DISAGREE HA=%llx HD=%llx pow=%d  ab=%d(%s) retro=%s\n",
                (unsigned long long)HA,(unsigned long long)HD,power,v,
                abr==WIN?"WIN":abr==LOSS?"LOSS":"DRAW", rr==WIN?"WIN":rr==LOSS?"LOSS":"DRAW"); }
    }
    printf("\ncards=%d cases=%d  agree=%ld disagree=%ld skip(abort/deg)=%ld\n",NCARDS,NCASE,agree,disagree,skip);
    printf(disagree?"*** SOLVERS DISAGREE ***\n":"OK: retrograde matches alpha-beta on all resolved cases\n");
    return 0;
}
