#!/bin/bash
# load.sh build | run LABEL HOST PAIRS TMIN TMAX GAMES DUR_MS | bot LABEL HOST TABLES DUR_MS | rematch LABEL HOST PAIRS WAIT_MS GAMES | push
# On the c7g load box. build: the load clients from src.tgz (foolish f99698319, whose c_base/bench/nano and client are
# byte-identical to 400a65d09's): p3load (bench/nano/p3load3.c), rematch (p3load with P3REMATCH_MS) and tlsres (for the nano).
# run: PAIRS two-human pairs (as the morning's run). bot: TABLES tables of one human and one octogen (P3BOT, botload.sh's ids).
# rematch: PAIRS pairs that play a first game, then rematch it (ASK_CONTINUE WAIT_MS after its end) GAMES-1 times;
# WAIT_MS may be a comma list, pair i taking the (i mod n)th.
M=/opt/mt; R=/var/tmp/mt; mkdir -p $R; B=$(cat $M/bucket); E=https://s3.dualstack.us-east-1.amazonaws.com
summ() {
python3 - "$1" <<'PY'
import sys,glob
o=sys.argv[1]; m=[]; c=[]
for f in glob.glob(o+'/rdv*/ev.*'):
    for l in open(f):
        p=l.split()
        if p and p[0]=='M': m.append((int(p[3]),int(p[4])))
        if p and p[0]=='C': c.append((int(p[2]),(int(p[4])-int(p[3]))/1000))
w=(max(e for s,e in m)-min(s for s,e in m))/1e6 if m else float('nan')
lat=sorted((e-s)/1000 for s,e in m)
pc=lambda v,q: v[min(len(v)-1,int(q*(len(v)-1)+.5))] if v else float('nan')
sf=sum(1 for f in glob.glob(o+'/*.err') if 'failed' in open(f).read())
print(f"moves={len(m)} window_s={w:.2f} moves_per_s={len(m)/w if m else 0:.1f} move_p50_ms={pc(lat,.5):.2f} move_p99_ms={pc(lat,.99):.2f} err_files={sf}")
for W in sorted(set(x for x,_ in c)):
    v=sorted(d for x,d in c if x==W)
    print(f"rematch wait_ms={W} n={len(v)} p50_ms={pc(v,.5):.3f} p90_ms={pc(v,.9):.3f} max_ms={v[-1]:.3f} mean_ms={sum(v)/len(v):.3f}")
PY
}
go_wait() {   # NCLIENTS PIDS...
  local need=$1; shift; local pids="$*" t=0 n dead alive
  while :; do
    n=$(ls rdv*/ready.* 2>/dev/null | wc -l); [ $n -ge $need ] && break
    dead=0; for p in $pids; do kill -0 $p 2>/dev/null || dead=$((dead+1)); done
    [ $((n+dead)) -ge $need ] && break
    sleep 0.2; t=$((t+1)); [ $t -gt 1500 ] && break
  done
  date +%s.%N > t_go
  for d in rdv*; do : > $d/go; done
  while :; do
    n=$(ls rdv*/done.* 2>/dev/null | wc -l); alive=0; for p in $pids; do kill -0 $p 2>/dev/null && alive=1; done
    [ $n -ge $need ] && break; [ "$alive" = 0 ] && break
    sleep 0.2
  done
  date +%s.%N > t_done
  for d in rdv*; do : > $d/quit; done
  wait
}
case $1 in
build)
  dnf install -y gcc make openssl-devel sqlite-devel tar gzip python3 > $R/dnf.log 2>&1
  mkdir -p /w && tar xzf $M/src.tgz -C /w && cd /w/c_base || exit 1
  make -s -j8 build/cbase > $R/build.log 2>&1 || { tail -30 $R/build.log; exit 1; }
  CAPS=$(make -s --no-print-directory -C ../foolish/c print-human-caps)
  F="-std=gnu11 -D_GNU_SOURCE -O2 -g -Icore/include -Icore/src -Iproviders/local/src -Icore/test -I../foolish/c/src -I../shared/c -pthread $CAPS"
  libs=$(find build/obj/cbase -name "*.o" ! -path "*/test/*" ! -name "main_*.o" ! -path "*/tools/*" ! -path "*/bench/*" ! -path "*/migrate/*")
  kern=$(find build/obj/kernel -name "*.o")
  cc $F -c bench/nano/lib_client_host.c -o /tmp/lch.o || exit 1
  for c in p3load:bench/nano/p3load3.c rematch:$M/rematch.c; do
    cc $F -c ${c#*:} -o /tmp/${c%%:*}.o && cc /tmp/${c%%:*}.o /tmp/lch.o $libs $kern -o $M/${c%%:*} -lsqlite3 -lssl -lcrypto -lpthread -lm || exit 1
  done
  cc -O2 -Wall -o $M/tlsres $M/tlsres.c || exit 1
  aws s3 cp --quiet $M/tlsres s3://$B/bin/tlsres --endpoint-url $E
  ls -l $M/p3load $M/rematch $M/tlsres; echo BUILT ;;
run)
  l=$2; host=$3; pairs=$4; tmin=$5; tmax=$6; games=$7; dur=$8
  out=$R/$l; rm -rf $out; mkdir -p $out; cd $out; ulimit -n 65536; i=0; pids=""
  while [ $i -lt $pairs ]; do
    mkdir -p rdv$i; a=$(( i / 250 % 250 + 1)); b=$(( i % 250 ))
    P3HOST=$host P3VIEWER=1.$a.$b.1:4$((i%1000)) P3DURATION_MS=$dur $M/p3load 8080 A${l}x$i a rdv$i $tmin $tmax $((i*2+11)) $games 0 > a$i.out 2> a$i.err & pids="$pids $!"
    P3HOST=$host P3VIEWER=1.$a.$b.2:5$((i%1000)) P3DURATION_MS=$dur $M/p3load 8080 B${l}x$i b rdv$i $tmin $tmax $((i*2+12)) $games 0 > b$i.out 2> b$i.err & pids="$pids $!"
    i=$((i+1))
  done
  go_wait $((pairs*2)) $pids; summ $out; cat t_go t_done; grep -h . *.err | grep -v "over at\|refused_at_once\|deadline in" | sort | uniq -c | sort -rn | head -3 ;;
bot)
  l=$2; host=$3; n=$4; dur=$5
  IDS=4f6f2ebb-1d3e-4761-a5a9-c91313eba087,f60b53ec-74f6-45fe-9e0f-315e5495bc18,45f7a264-aedc-4ee4-8473-4681d6e0bcb8,acc775ae-6d35-41d2-88bc-7dc1599cff82,df16e0de-4569-4771-804e-98f8d13affd1,fc185071-a522-424e-a1bd-0c4d34b32d3e,abe1fcfa-992c-4bf5-b163-32c0b0a21a7b
  out=$R/$l; rm -rf $out; mkdir -p $out; cd $out; ulimit -n 65536; i=0; pids=""
  while [ $i -lt $n ]; do
    mkdir -p rdv$i; a=$(( i / 250 % 250 + 1)); b=$(( i % 250 ))
    P3HOST=$host P3BOT=1 P3SEATS=2 P3BOTIDS=$IDS P3VIEWER=1.$a.$b.1:4$((i%1000)) P3DURATION_MS=$dur $M/p3load 8080 A${l}x$i a rdv$i 0 1 $((i*2+11)) 100000 0 > a$i.out 2> a$i.err & pids="$pids $!"
    i=$((i+1))
  done
  go_wait $n $pids; summ $out; cat t_go t_done; grep -h . *.err | grep -v "over at\|refused_at_once\|deadline in" | sort | uniq -c | sort -rn | head -3 ;;
rematch)
  l=$2; host=$3; pairs=$4; wms=$5; games=$6
  out=$R/$l; rm -rf $out; mkdir -p $out; cd $out; ulimit -n 65536; i=0; pids=""
  while [ $i -lt $pairs ]; do
    mkdir -p rdv$i; a=$(( i / 250 % 250 + 1)); b=$(( i % 250 )); IFS=, read -ra WS <<< "$wms"; w=${WS[$((i % ${#WS[@]}))]}
    P3REMATCH_MS=$w P3HOST=$host P3VIEWER=1.$a.$b.1:4$((i%1000)) $M/rematch 8080 A${l}x$i a rdv$i 0 1 $((i*2+11)) $games 0 > a$i.out 2> a$i.err & pids="$pids $!"
    P3REMATCH_MS=$w P3HOST=$host P3VIEWER=1.$a.$b.2:5$((i%1000)) $M/rematch 8080 B${l}x$i b rdv$i 0 1 $((i*2+12)) $games 0 > b$i.out 2> b$i.err & pids="$pids $!"
    i=$((i+1))
  done
  go_wait $((pairs*2)) $pids; summ $out; cat t_go t_done; grep -h . *.err | grep -v "over at\|refused_at_once" | sort | uniq -c | sort -rn | head -3 ;;
push) aws s3 cp --quiet --recursive $R s3://$B/res/load/ --endpoint-url $E --exclude "*/rdv*/go" ; echo pushed ;;
esac
