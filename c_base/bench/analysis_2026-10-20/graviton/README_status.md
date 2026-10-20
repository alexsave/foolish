# Final real-Graviton measurement - RUNNING ON ITS OWN IN AWS (launched 2026-10-20 12:10 UTC)

The full matrix runs without the laptop: 45 runs (3 rounds x 5 scenarios x BASE/NOW/NOW+uring, all rerun on the new boxes for one consistent set), then a perf record of hum2 on NOW, then the IPC side runs (hum2 and sat2 x 3 builds x 3 rounds on a c6g.large server).
Every run's stop saves smaps_rollup.txt, so the counted NOW sat2 runs carry the smaps split.
The driver is master.sh on the load box (systemd unit cbmaster). It runs one drive.py per run and uploads each run's tarball, results.txt, drive.log and master.log to the bucket as the run finishes. At the end it powers off both servers and itself.
Backstop: every box has InstanceInitiatedShutdownBehavior=terminate and `shutdown -h +240`, so all are gone by about 16:10 UTC.

- Bucket: cbase-perf-temp-3d33571b. SG: sg-0a601e54cb30facdc.
- srv i-06210f16565c44f32 (t4g.nano, unlimited credits, 2600:1f18:e01:9f00:5876:54c6:59f0:90b9)
- ipc i-09775323cffb891f3 (c6g.large, 2600:1f18:e01:9f00:f8ad:1f:abcd:d223)
- load i-0b352b1567b840733 (c7g.2xlarge)

## Collect and clean up (all that has to happen when the laptop is back)

A separate calibration agent runs its own instances tagged `cbase-perf-temp-cal-*` (a c6g.large and a t4g.nano) with its own perf-temp-cal/ prefix or bucket.
Never use a `cbase-perf-temp-*` wildcard in a filter or a delete here: every command below names this run's exact instance IDs, bucket and SG only.

```
#!/bin/bash
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
SP=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad
B=cbase-perf-temp-3d33571b; SG=sg-0a601e54cb30facdc
aws sts get-caller-identity
# 1. fetch: master.log ends with "ALL FINISHED" when the driver completed
aws s3 sync --only-show-errors s3://$B/ $SP/final_graviton/s3b/ --exclude pkg.tgz
tail -5 $SP/final_graviton/s3b/master.log
mkdir -p $SP/final_graviton/runs2 && for f in $SP/final_graviton/s3b/runs/*.tgz; do tar xzf $f -C $SP/final_graviton/runs2; done
python3 $SP/fg/agg2.py $SP/final_graviton/s3b/results.txt     # ipc_* labels need agg2's regex widened
# 2. credits, before and after (CloudWatch keeps them; the boxes could not read it)
for m in CPUCreditBalance CPUSurplusCreditBalance CPUSurplusCreditsCharged CPUUtilization; do echo "== $m"
  aws cloudwatch get-metric-statistics --namespace AWS/EC2 --metric-name $m --dimensions Name=InstanceId,Value=i-06210f16565c44f32 \
    --start-time 2026-10-20T12:05:00Z --end-time 2026-10-20T16:30:00Z --period 300 --statistics Maximum \
    --query 'sort_by(Datapoints,&Timestamp)[].[Timestamp,Maximum]' --output text; done > $SP/final_graviton/credits_srv.txt
# 3. make sure every temp box is gone (explicit ids: zsh does not word-split)
aws ec2 terminate-instances --instance-ids i-06210f16565c44f32 i-09775323cffb891f3 i-0b352b1567b840733 >/dev/null 2>&1
aws ec2 wait instance-terminated --instance-ids i-06210f16565c44f32 i-09775323cffb891f3 i-0b352b1567b840733
aws ec2 describe-instances --instance-ids i-06210f16565c44f32 i-09775323cffb891f3 i-0b352b1567b840733 --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text   # all three must say terminated
# 4. delete the temp bucket (with its objects) and the SG; perf-temp/ in the code bucket was never used
aws s3 rm --only-show-errors --recursive s3://$B/ && aws s3api delete-bucket --bucket $B
aws ec2 delete-security-group --group-id $SG
aws s3 ls s3://cbase-code-<acct>-us-east-1/perf-temp/ | wc -l    # 0
aws ec2 describe-instances --instance-ids i-08f26ffb900294ee8 --query 'Reservations[].Instances[].State.Name' --output text   # production: running, untouched
```

Then write the report below (methodology, a table per scenario, the profile, the smaps split, IPC from the ipc_* runs, anything odd).

---

# Final real-Graviton measurement - PAUSED (2026-10-20 12:07 UTC)

## Status

Paused on the coordinator's request.
Every temp resource is gone: instances i-0b6b1ffb045c273ac (srv, t4g.nano), i-0395fb1fe4b04ed62 (load, c7g.2xlarge), i-06beb7a3775dbadb3 (ipc, c6g.large) and the two from the first failed launch are terminated.
Bucket cbase-perf-temp-f3b98ca1 and security group sg-0a29a31ff3d7103ab are deleted, and nothing is under perf-temp/ in the code bucket.
`describe-instances` shows no cbase-perf-temp-* instance pending, running, stopping or stopped.
Production (i-08f26ffb900294ee8) was not touched and is still running.

What is measured (raw output in final_graviton/runs/, per-run lines in final_graviton/s3/results.txt, the driver log in final_graviton/s3/drive.log):
- Round 1 of hum2 for BASE, NOW and NOW+uring (io_uring confirmed from "listening on :::8080 (io_uring)").
- Round 1 of sat2 for BASE.
- The perf record -g (dwarf) of hum2 on NOW: final_graviton/runs/prof_hum2_NOW/srv_tar/prof_hum2_NOW/rec_self.txt, rec_children.txt and rec_dso.txt.
- A smoke sat2 run of NOW+uring (smoke1): not one of the 3 counted runs, but it has a smaps_rollup.
- CPU credits so far: final_graviton/credits_srv_part1.txt.

What is left:
- Rounds 1-3 of the rest of the matrix: 41 of the 45 runs.
- The IPC side run on c6g.large (see the finding below).
- The smaps split at the end of a NOW sat2 run (every run's stop already saves smaps_rollup.txt).
- The credits after the matrix.
- The write-up.

## Findings so far that the rerun must respect

- **No IPC on the t4g.nano.** A burstable instance exposes only the cycle counter: `instructions`, r08 and r1b are not supported, and armv8_pmuv3_0/events lists only cpu_cycles.
  A non-burstable Graviton exposes the full PMU (the c7g measured IPC 2.85 on dd).
  The plan is to run the IPC side measurement with the server on a c6g.large, which has the same Neoverse N1 core as the t4g.
  Plan it after the main matrix, with the same load box; the srvctl daemon already runs there when launched with up3.sh.
- **No cpuidle sysfs on the t4g.nano**, because the guest has no cpuidle driver.
  Idle exits are therefore counted from tracepoint power:cpu_idle (state == -1, the exit), across both vCPUs, by the bpftrace meter.
- **The root filesystem is XFS**, so the per-file byte meter is tracepoint xfs:xfs_file_buffered_write by inode; ext4_file_write_iter does not exist.
  The journal is mmapped, so it never shows up as writes.
  Its pages are counted by writeback:writeback_dirty_folio.
  That counter reads about 1 B/move for NOW, which looks too low (the journal is about 190 logical B/record).
  Check this before trusting the journal column; the 8 MB segments may already be dirty or prefaulted.
  The windowed device bytes from nvme diskstats are the robust number.
- **The load box is a c7g.2xlarge, not a nano.** A nano load box could not saturate the server; round 2 only ran 8 pairs at saturation.
  NOW+uring sat2 used 1.36 cores of the server's 2 vCPUs.
- **The server runs with production defaults** (no CBASE_REQ_WORKERS override, unlike the Docker rig's =1), on the instance's root gp3 volume.
- **The server_cpu_ms_per_move=nan bug is fixed** by not using the old summ.py at all.
  fin2.py takes server CPU from the server's own perf stat -I intervals and from the /proc ticks, inside the same window as the moves.
- **The first launch's SSM agent could not reach SSM.** The subnet is IPv6-only, and the agent needs `{"Agent":{"UseDualStackEndpoint":true}}`.
  dnf was also OOM-killed on the nano: install perf and bpftrace under a temporary 1 GB swapfile.
  up2.sh does both.
- **SSM kills nohup'd children.** Start long jobs with systemd-run, as go.sh and push.sh now do.

## Partial numbers (round 1 only, one run each - not final)

hum2: 192 two-player games at human pace, t4g.nano, 37 s window.

| metric | BASE | NOW | NOW+uring |
|---|---|---|---|
| task-clock µs/move | 265.0 | 84.1 | 80.7 |
| cycles/move | 484k | 148k | 142k |
| syscalls/move | 23.4 | 3.93 | 1.98 |
| context switches/move | 3.04 | 0.96 | 0.97 |
| cpu-migrations/move | 0.140 | 0.002 | 0.001 |
| idle exits/move (2 vCPUs) | 9.0 | 6.7 | 6.8 |
| moves/s | 98 | 98 | 97 |
| move p50/p95/p99 ms | 0.43/0.59/0.73 | 0.26/0.34/0.37 | 0.25/0.33/0.37 |
| push p50/p95/p99 ms | 0.43/0.57/0.68 | 0.26/0.35/0.37 | 0.25/0.33/0.36 |
| lost pushes | 0 | 0 | 0 |
| WAL B/move | 9360 | 0 | 0 |
| device bytes written/move | 9663 | 1332 | 1435 |

sat2 BASE r1: 9381 moves/s, 152.6 task-clock µs/move, 27.8 syscalls/move.
smoke sat2 NOW+uring: 17688 moves/s, 77.0 µs/move, 5.68 syscalls/move, RSS 67.5 MB, of which 55.0 MB anonymous and 12.5 MB file-backed.

## How to resume

Scripts are in scratchpad/fg/.
The package is scratchpad/fg/pkg/: serve-BASE and serve-NOW (unstripped static arm64 builds of 5d5ef505b and 4aa1c22e3, from final/rig/bin), p3load2-linux, the rig's test secrets, srvctl.py, drive.py and fin2.py.

```
FG=/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/fg
aws sts get-caller-identity
# 1. a new temp bucket + SG: run the first half of $FG/up.sh (its bucket/SG part) or simply $FG/up.sh,
#    which then launches with the OLD user data: terminate those two and use up2.sh instead. Simplest:
#    edit up.sh's user data the way mk2.py does (python3 $FG/mk2.py writes up2.sh from up.sh), then:
$FG/up.sh            # creates bucket + SG (writes bucket.txt, sg.txt) - then terminate its 2 instances
: > $FG/ids.txt; $FG/up2.sh   # srv t4g.nano (unlimited) + load c7g.2xlarge, dual-stack SSM, swap for dnf, shutdown -h +150
$FG/up3.sh           # optional: the c6g.large IPC server (tag cbase-perf-temp-ipc)
# 2. update the instance ids in go.sh (LOAD=), wait.sh, and the server IPv6 below; wait for /var/tmp/cb/READY via ssm.sh
$FG/push.sh <srv-id> <load-id> [<ipc-id>]   # ships the latest scripts, restarts srvctl under systemd-run
# 3. the matrix: drop the 4 finished runs (or rerun all 45 for one consistent set)
P=""; for r in 1 2 3; do for sc in hum2 sat2 hum8 sat8 bot; do P="$P${sc}_BASE_r$r BASE 0 $sc\n${sc}_NOW_r$r NOW 0 $sc\n${sc}_NOWu_r$r NOW 1 $sc\n"; done; done
$FG/go.sh <srv-ipv6> "$P"; $FG/wait.sh 45
# 4. IPC: $FG/go.sh <ipc-ipv6> "ipc_hum2_BASE_r1 BASE 0 hum2\nipc_hum2_NOW_r1 NOW 0 hum2\nipc_sat2_BASE_r1 BASE 0 sat2\nipc_sat2_NOW_r1 NOW 0 sat2\n"
# 5. pull: aws s3 sync s3://<bucket>/ final_graviton/s3/ ; untar runs/*.tgz into final_graviton/runs ; python3 $FG/agg2.py final_graviton/s3/results.txt
# 6. credits (CloudWatch CPUCreditBalance / CPUSurplusCreditBalance / CPUSurplusCreditsCharged on the srv id), then
#    terminate this run's instances BY EXACT ID (never a cbase-perf-temp-* wildcard: the calibration agent's cbase-perf-temp-cal-* boxes would match), delete bucket + SG, describe-instances by id.
```

Rough timing: 384-528 clients sign up in about 15 s.
A run takes about 65-75 s, so the matrix takes about 55 min and fits in the 150 min self-termination budget.
