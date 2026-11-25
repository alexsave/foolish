#!/bin/bash
# ctest.sh: local dry check of trip.bt, cpu.sh (server and load samplers, report, report2, lreport, irqsnap) and prof.sh info
# in a privileged arm64 amazonlinux:2023 container (run by: docker run ... bash /scripts/ctest.sh). Not shipped.
dnf install -y -q bpftrace python3 procps-ng util-linux perf >/dev/null 2>&1
mount -t tracefs tracefs /sys/kernel/tracing; mount -t debugfs debugfs /sys/kernel/debug
mkdir -p /opt/mt /var/tmp/mt; cp /pkg/*.sh /pkg/trip.bt /opt/mt/; echo dummy > /opt/mt/bucket
# a threaded workload: socketpair send/recv, epoll, futex (Condition), sleeps
python3 - <<'PY' &
import threading,socket,select,time
a,b=socket.socketpair(); ep=select.epoll(); ep.register(b.fileno(),select.EPOLLIN)
cv=threading.Condition(); stop=time.time()+14
def snd():
    while time.time()<stop: a.send(b'x'*100); time.sleep(0.0005)
def rcv():
    while time.time()<stop:
        for fd,e in ep.poll(0.05): b.recv(4096)
        with cv: cv.notify()
def waiter():
    while time.time()<stop:
        with cv: cv.wait(0.01)
ts=[threading.Thread(target=f) for f in (snd,rcv,waiter)]
[t.start() for t in ts]; [t.join() for t in ts]
PY
P=$!; echo $P > /var/tmp/mt/pid_t1; sleep 0.3
bash /opt/mt/cpu.sh sample t1; bash /opt/mt/cpu.sh lsample t1; bash /opt/mt/cpu.sh irqsnap t1 start
T0=$(date +%s.%N)
timeout -s INT 6 bpftrace /opt/mt/trip.bt $P > /tmp/bt.txt 2>/tmp/bt.err; echo bt_rc=$?
grep -v WARNING /tmp/bt.err | grep -v '^ ' | head -5; head -2 /tmp/bt.txt; echo "ticks $(grep -c '^TICK' /tmp/bt.txt) lines $(wc -l < /tmp/bt.txt)"
awk '/^TICK/{n++} n==3' /tmp/bt.txt | head -60
sleep 2; T1=$(date +%s.%N); bash /opt/mt/cpu.sh irqsnap t1 end; bash /opt/mt/cpu.sh lstop t1
bash /opt/mt/cpu.sh report t1 $T0 $T1 1000; bash /opt/mt/cpu.sh report2 t1 $T0 $T1 1000; bash /opt/mt/cpu.sh lreport t1 $T0 $T1
grep -c . /var/tmp/mt/cpu_t1.log; grep '^Q' /var/tmp/mt/cpu_t1.log | head -3; grep '^C' /var/tmp/mt/cpu_t1.log | head -2; grep '^R' /var/tmp/mt/cpu_t1.log | head -2; ls /var/tmp/mt
wait
perf --version
