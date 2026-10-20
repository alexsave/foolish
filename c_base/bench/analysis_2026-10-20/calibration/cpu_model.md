# CPU model: Docker arm64 rig -> t4g.nano (Graviton2 / Neoverse N1)

Status (2026-10-20 12:30Z): STUB.
The AWS calibration run is launched and uploading; the model, the published-data citations, the spec sheet and the rig run are owed on return.

## What is running

- Suite: `scratchpad/cal/calbench.c`, one static musl arm64 binary (`scratchpad/cal/bin/calbench`), built by `scratchpad/cal/build.sh` in alpine 3.24 (the same image as c_base/bench/nano/build.sh) with cbase serve's flags: `-std=gnu11 -D_GNU_SOURCE -O2 -g -pthread -static`, no `-mcpu` (the serve default).
- Groups: env, clock (timing-only cycle model: 256-add chain for GHz, fp add/mul/div/sqrt s+d dep and 8-wide, sdiv sweep), core (int add/mul dep/ind, udiv/sdiv by operand width, branches, atomics, mutex, contended), kernel (null syscalls, clock_gettime, pipe/eventfd, epoll, page faults first-touch vs reuse, pwrite/pread/fdatasync/msync, io_uring), net (TCP loopback send-only/recv-only/send+recv by payload 16 B..64 KB, fan-out to 1..16 sockets, futex/pipe/eventfd ping-pong same- and cross-core), wake (futex and epoll wake-up after 0/1/10/100 ms idle: latency, waker syscall, wakee CPU, sleep overshoot), real (HMAC-SHA256/SHA256 by length with OpenSSL 3.5, SQLite 3.53 WAL+NORMAL insert/update/select/txn by row size), str (memcpy/memset/memmove/memcmp/strcmp/strlen by length x 4 alignments, strcmp/memcmp by first-difference position), malloc (musl mallocng by size class, single, batch-64, and with a second thread contending), mem (pointer chase 4 KB..256 KB..256 MB, THP default and off), stream (copy/scale/add/triad), ebs (O_DIRECT QD1 latency, io_uring QD4/16/32 IOPS, 1 MB sequential), steal (frequency and /proc/stat steal every 250 ms, 30 s idle then 90 s with the other CPU busy; passes 1 and last only).
- Output: TSV `group name param k n min max mean std p50 p99 cyc insn brmiss l1dmiss llcmiss cpu pass`, ns per op; each sample times k back-to-back ops (k=1 rows are true per-op distributions). Main thread pinned to CPU 1 (`CAL_CPU`); cross-core partner on CPU 0.
- Boxes (tag Name=cbase-perf-temp-cal-*, own SG `cbase-perf-temp-cal-sg`, own bucket):
  - c6g.large `i-0bb7f166f7f2d75fe`
  - t4g.nano `i-0f211355212bca40a`, CpuCredits=unlimited
  - InstanceInitiatedShutdownBehavior=terminate, `shutdown -h +185` plus a systemd `poweroff` timer at 190 min, and the runner's own `shutdown -h now` at the end.
  - Runner: `scratchpad/cal/pkg/runner.sh` under `systemd-run --unit cal-runner`: 5 passes, 15 min apart, upload after every group.
- Bucket: `cbase-perf-temp-cal-258b7d0a` (in `scratchpad/cal/bucket.txt`), prefix `perf-temp-cal/<role>-<instance-id>/`.

## First numbers (pass 1)

- add_chain256: 102.43 ns per 256 dependent adds on both boxes = 0.4001 ns/cycle = 2.50 GHz, c6g and t4g identical to the ns.
- fadd_d dep 12.81 ns / 16 = 2 cycles; mul_dep 12.82 ns / 8 = 4 cycles... (wait: mul x8 = 12.82 ns = 32 cycles = 4 cycles each? N1 MUL latency is 2-3; to check on return).
- getpid: 114 ns both. eventfd write+read: 408-413 ns both.

## Known defects to fix on return

- The PMU columns on c6g are unscaled under multiplexing (5 separate events, no `PERF_FORMAT_TOTAL_TIME_ENABLED/RUNNING`): add_chain256 shows ~104 insn per 256-add iteration, which is wrong. Fix: one event group of cycles+instructions with scaling, or read only 2 counters.
  The t4g reports pmu=1 but every counter reads -1 (open succeeds for cycles, reads fail): as expected, no PMU there.
- The foolish kernel functions (state encode/decode, evwire) are not in the suite yet.

## Collect (first step on return)

```bash
#!/bin/bash
C=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/cal
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
aws sts get-caller-identity --query Arn --output text
B=$(cat $C/bucket.txt)
aws s3 sync --only-show-errors s3://$B/perf-temp-cal/ $C/s3/ --exclude pkg.tgz
cat $C/s3/*/runner.log          # expect "ALLDONE" on both
aws ec2 describe-instances --instance-ids i-0bb7f166f7f2d75fe i-0f211355212bca40a --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text
```

## Cleanup (only the cal resources; never a cbase-perf-temp-* wildcard)

```bash
#!/bin/bash
C=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/cal
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
aws ec2 terminate-instances --instance-ids i-0bb7f166f7f2d75fe i-0f211355212bca40a   # no-op if already terminated
aws ec2 wait instance-terminated --instance-ids i-0bb7f166f7f2d75fe i-0f211355212bca40a
aws ec2 delete-security-group --group-id $(cat $C/sg.txt)
B=$(cat $C/bucket.txt)   # cbase-perf-temp-cal-258b7d0a, exactly this name
aws s3 rm --recursive s3://$B/ && aws s3api delete-bucket --bucket $B
```

## Rig side (first step on return, not run yet)

Run the same binary in the nano rig's container setup (alpine arm64, `--cpus=2`, as c_base/bench/nano):

```bash
#!/bin/bash
C=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/cal
mkdir -p $C/rig
docker run --rm --cpus=2 --name cal-rig --platform linux/arm64 -e CAL_CPU=1 -e CAL_DIR=/tmp \
  -v $C/bin:/b:ro -v $C/rig:/o alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 \
  sh -c 'for p in 1 2 3; do for g in env clock core kernel net wake real str malloc mem stream; do /b/calbench $g $p > /o/p${p}_$g.tsv; done; sleep 300; done'
```

Note the smoke run in Docker showed cross-core futex/pipe/eventfd ping-pong at ~30-50 us round trip against ~2 us same-core: the Docker VM's vCPU wake-up is a large rig-only cost the model must carry separately.

## Owed on return

- Published data with citations (Neoverse N1 Software Optimization Guide, AWS Graviton2 docs, aws-graviton-getting-started, published lmbench/syscall numbers; Apple P/E core and the Docker Linux VM).
- Curve fits per primitive (fixed + per-byte, piecewise at cache knees) and the rig -> nano transform weighted by the profile in scratchpad/final_graviton/ and the rig's `sample` profiles.
- The side-by-side t4g.nano / c6g.large spec sheet from `spec_*.txt`, the steal/freq series and the ebs/stream groups.
